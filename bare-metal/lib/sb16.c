/* bare-metal/lib/sb16.c
 * Sound Blaster 16 audio driver — 16-bit signed stereo via DMA channel 5.
 *
 * Uses the secondary 8237A DMA controller (channel 5, 16-bit) which is the
 * SB16's "native" high-quality mode and the most-exercised code path in
 * QEMU's SB16 emulation.
 *
 * Audio format: signed 16-bit stereo at SB16_SAMPLE_RATE Hz.
 * DMA buffer: 8 KB total, 32 KB-aligned (must not cross a 128 KB boundary
 *             for 16-bit DMA word-address mode).
 *
 *   [   half 0   |   half 1   ]
 *    0        4095 4096     8191   (byte offsets)
 *
 * Requires irq_init() (PIC remap) to have been called first.
 * Interrupt acknowledgement is done by polling from the game loop via
 * irq_ack_sb16().
 */

#include "bare.h"
#include "sb16.h"
#include "irq_hw.h"
#include "ym2612.h"
#include "psg.h"

/* ---- I/O helpers --------------------------------------------------------- */

static inline void outb(uint16_t port, uint8_t v)
{
    __asm__ volatile ("outb %0, %1" : : "a"(v), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t v;
    __asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline void io_wait(void) { outb(0x80u, 0u); }

/* ---- SB16 DSP port map (base 0x220) -------------------------------------- */

#define SB_BASE       0x220u
#define SB_RESET      (SB_BASE + 0x6u)
#define SB_READ       (SB_BASE + 0xAu)
#define SB_WRITE      (SB_BASE + 0xCu)
#define SB_READ_STAT  (SB_BASE + 0xEu)    /* 8-bit IRQ ACK              */
#define SB_READ16_ACK (SB_BASE + 0xFu)    /* 16-bit IRQ ACK             */
#define SB_MIXER_ADDR (SB_BASE + 0x4u)
#define SB_MIXER_DATA (SB_BASE + 0x5u)

/* ---- ISA DMA — channel 5 (secondary 8237A, 16-bit) ---------------------- */
/*
 * The secondary 8237A handles 16-bit DMA channels 4-7.
 * Channel 5 = "channel 1" of the secondary controller.
 * Port addresses: secondary base + 2*offset for address/count registers.
 *
 * 16-bit DMA word-address mode:
 *   physical_byte_addr = (page << 16) | (word_addr << 1)
 *   word_addr  = (byte_addr >> 1) & 0xFFFF
 *   word_count = (byte_count / 2) - 1
 */

#define DMA2_ADDR5  0xC4u   /* ch5 base word-address (flip-flop)       */
#define DMA2_CNT5   0xC6u   /* ch5 base word-count   (flip-flop)       */
#define DMA2_CMD    0xD0u   /* secondary command (write) / status (read)*/
#define DMA2_MASK   0xD4u   /* secondary single-channel mask            */
#define DMA2_MODE   0xD6u   /* secondary mode register                  */
#define DMA2_FLIP   0xD8u   /* secondary clear byte-ptr flip-flop       */
#define DMA_PAGE5   0x8Bu   /* page register for channel 5              */

/* ---- DMA buffer ---------------------------------------------------------- */

/* 1024 stereo frames per half, 4 bytes per frame (16-bit L + 16-bit R) */
#define HALF_FRAMES   1024u
#define HALF_BYTES    (HALF_FRAMES * 4u)   /* 4096 bytes per half       */
#define DMA_BYTES     (HALF_BYTES  * 2u)   /* 8192 bytes total          */

/* Must be aligned to DMA_BYTES so it doesn't cross a 128 KB boundary
 * (the page register covers 128 KB for 16-bit DMA: page<<16 gives the
 * 128 KB-aligned base, word_addr<<1 indexes within it). */
static uint8_t dma_buf[DMA_BYTES] __attribute__((aligned(DMA_BYTES)));

/* ---- State --------------------------------------------------------------- */

static int sb16_ok = 0;

/* ---- DSP helpers --------------------------------------------------------- */

static inline void mixer_write(uint8_t reg, uint8_t val)
{
    outb(SB_MIXER_ADDR, reg);
    io_wait();
    outb(SB_MIXER_DATA, val);
    io_wait();
}

static void sb16_mixer_init(void)
{
    mixer_write(0x00u, 0x00u);   /* reset mixer to defaults      */

    /* Tell the SB16 which IRQ and DMA channels to use.
     * Without these, QEMU's SB16 may not know where to DMA from. */
    mixer_write(0x80u, 0x02u);   /* IRQ select: IRQ5             */
    mixer_write(0x81u, 0x22u);   /* DMA select: DMA1(8) + DMA5(16) */

    mixer_write(0x22u, 0xFFu);   /* SBPro master volume: max     */
    mixer_write(0x04u, 0xFFu);   /* SBPro DAC/PCM volume: max    */
    mixer_write(0x26u, 0xFFu);   /* SBPro FM volume: max         */
    mixer_write(0x30u, 0xF8u);   /* SB16 master left             */
    mixer_write(0x31u, 0xF8u);   /* SB16 master right            */
    mixer_write(0x32u, 0xF8u);   /* SB16 voice left              */
    mixer_write(0x33u, 0xF8u);   /* SB16 voice right             */
}

static int dsp_reset(void)
{
    outb(SB_RESET, 1u);
    for (int i = 0; i < 16; i++) io_wait();
    outb(SB_RESET, 0u);
    for (int i = 0; i < 4000; i++) {
        if ((inb(SB_READ_STAT) & 0x80u) && inb(SB_READ) == 0xAAu)
            return 1;
        io_wait();
    }
    return 0;
}

static void dsp_write(uint8_t v)
{
    for (int i = 0; i < 100000; i++) {
        if (!(inb(SB_WRITE) & 0x80u)) {
            outb(SB_WRITE, v);
            return;
        }
    }
}

static uint8_t dsp_read(void)
{
    for (int i = 0; i < 100000; i++) {
        if (inb(SB_READ_STAT) & 0x80u)
            return inb(SB_READ);
        io_wait();
    }
    return 0xFFu;
}

/* ---- DMA programming (16-bit, channel 5) ---------------------------------
 *
 * Secondary 8237A uses WORD addresses.  Physical byte address:
 *   phys = (page << 16) | (word_addr << 1)
 *
 * Mode 0x59 = single-transfer | addr-increment | auto-init | read | ch5
 *   (channel 5 on secondary = channel 1 index, so bits[1:0] = 01)
 * ----------------------------------------------------------------------- */

static void dma_program(void)
{
    uint32_t addr  = (uint32_t)(uintptr_t)dma_buf;
    uint8_t  page  = (uint8_t)(addr >> 16);
    uint16_t waddr = (uint16_t)((addr >> 1) & 0xFFFFu);
    uint16_t wcnt  = (uint16_t)((DMA_BYTES / 2u) - 1u);

    kprintf("SB16 DMA16: phys=0x%lx page=0x%x waddr=0x%x wcnt=%u\n",
            (unsigned long)addr, (unsigned)page,
            (unsigned)waddr, (unsigned)wcnt);

    outb(DMA2_CMD,   0u);            /* enable secondary DMA controller */
    outb(DMA2_MASK,  0x05u);         /* mask ch5 (bit2=1, ch=01)        */
    outb(DMA2_FLIP,  0u);            /* clear flip-flop                 */
    outb(DMA2_MODE,  0x59u);         /* single/AI/read/ch1-of-secondary */
    outb(DMA2_ADDR5, (uint8_t)(waddr));
    outb(DMA2_ADDR5, (uint8_t)(waddr >> 8));
    outb(DMA2_CNT5,  (uint8_t)(wcnt));
    outb(DMA2_CNT5,  (uint8_t)(wcnt >> 8));
    outb(DMA_PAGE5,  page);
    outb(DMA2_MASK,  0x01u);         /* unmask ch5                      */
    kprintf("SB16 DMA16: programmed OK\n");
}

/* ---- Start DSP playback --------------------------------------------------
 *
 * 0xB6 = 16-bit auto-init DMA output, FIFO on.
 * Mode byte 0x30 = signed stereo.
 * Block size = number of 16-bit samples per interrupt - 1.
 * For stereo, each frame = 2 samples (L+R), so 1024 frames = 2048 samples.
 * ----------------------------------------------------------------------- */

static void sb16_start_playback(void)
{
    uint16_t rate = (uint16_t)SB16_SAMPLE_RATE;

    /* Block size for the DSP command = number of mono samples per interrupt - 1.
     * QEMU internally multiplies by 2 (stereo) and by 2 (16-bit bytes), so
     * HALF_FRAMES-1 → (HALF_FRAMES * 2 * 2) bytes = HALF_BYTES per interrupt. */
    uint16_t blk  = (uint16_t)(HALF_FRAMES - 1u);

    dsp_write(0xD1u);                       /* speaker on                */
    dsp_write(0x41u);                       /* set output sample rate    */
    dsp_write((uint8_t)(rate >> 8));
    dsp_write((uint8_t)(rate));
    dsp_write(0xB6u);                       /* 16-bit auto-init FIFO out */
    dsp_write(0x30u);                       /* signed stereo             */
    dsp_write((uint8_t)(blk));
    dsp_write((uint8_t)(blk >> 8));

    kprintf("SB16: playback — %u Hz 16-bit signed stereo, blk=%u\n",
            (unsigned)rate, (unsigned)(blk + 1u));
}

/* ---- Public API ---------------------------------------------------------- */

int sb16_init(void)
{
    /* Pre-fill with a 440 Hz square wave (signed 16-bit stereo).
     * If DMA works, you'll hear a brief buzz before game audio takes over. */
    int16_t *buf16 = (int16_t *)(void *)dma_buf;
    int total_samples = (int)(DMA_BYTES / sizeof(int16_t));
    for (int i = 0; i < total_samples; i++) {
        int frame = i / 2;
        buf16[i] = ((frame % 100) < 50) ? 16000 : -16000;
    }

    if (!dsp_reset()) {
        kprintf("SB16: DSP not detected at 0x%x\n", (unsigned)SB_BASE);
        return 0;
    }

    dsp_write(0xE1u);
    uint8_t major = dsp_read();
    uint8_t minor = dsp_read();
    kprintf("SB16: DSP v%u.%u  buf=0x%lx (%lu B)\n",
            (unsigned)major, (unsigned)minor,
            (unsigned long)(uintptr_t)dma_buf,
            (unsigned long)DMA_BYTES);

    if (major < 4u) {
        kprintf("SB16: DSP too old (need v4+), audio disabled\n");
        return 0;
    }

    sb16_mixer_init();
    dma_program();
    sb16_start_playback();

    sb16_ok = 1;
    return 1;
}

int sb16_available(void) { return sb16_ok; }

void sb16_render_frame(void)
{
    if (!sb16_ok) return;

    /* ACK any pending 16-bit SB16 interrupt + send PIC EOI */
    (void)inb(SB_READ16_ACK);
    irq_ack_sb16();

    static int accum = 0;
    static int fill  = 0;

    accum += 735;
    if (accum < (int)HALF_FRAMES)
        return;
    accum -= (int)HALF_FRAMES;

    fill ^= 1;

    int16_t *out = (int16_t *)(void *)(dma_buf + (fill * (int)HALF_BYTES));

    static int32_t mix[HALF_FRAMES * 2];
    bm_memset(mix, 0, sizeof(mix));
    ym2612_run_samples(mix, (int)HALF_FRAMES, (int)SB16_SAMPLE_RATE);
    psg_run_samples   (mix, (int)HALF_FRAMES, (int)SB16_SAMPLE_RATE);

    int32_t peak = 0;
    int nsamp = (int)(HALF_FRAMES * 2u);
    for (int i = 0; i < nsamp; i++) {
        int32_t s = mix[i];
        int32_t a = s < 0 ? -s : s;
        if (a > peak) peak = a;
        if (s >  32767) s =  32767;
        if (s < -32768) s = -32768;
        out[i] = (int16_t)s;
    }

    static int render_n = 0;
    if ((++render_n & 63) == 1)
        kprintf("SB16 render #%d  peak=%ld  half=%d\n",
                render_n, (long)peak, fill);
}

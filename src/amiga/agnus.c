/*
 * Agnus — Chapter 3: DMA control + PAL beam counters + Copper + Blitter.
 */

#include "agnus.h"
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Lifecycle                                                           */
/* ------------------------------------------------------------------ */

void agnus_init(agnus_t *ag)
{
    memset(ag, 0, sizeof(*ag));
    ag->bltafwm = 0xFFFF;
    ag->bltalwm = 0xFFFF;
}

void agnus_reset(agnus_t *ag)
{
    agnus_init(ag);
}

/* ------------------------------------------------------------------ */
/*  Beam counter                                                        */
/* ------------------------------------------------------------------ */

void agnus_tick_scanline(agnus_t *ag, int line)
{
    ag->line = line;
    ag->hpos = 0;
}

/* ------------------------------------------------------------------ */
/*  Helpers                                                             */
/* ------------------------------------------------------------------ */

static uint16_t ram_read16(const uint8_t *ram, uint32_t addr, uint32_t size)
{
    if (addr + 1u >= size) return 0;
    return (uint16_t)((ram[addr] << 8) | ram[addr + 1]);
}

static void ram_write16(uint8_t *ram, uint32_t addr, uint32_t size, uint16_t val)
{
    if (addr + 1u >= size) return;
    ram[addr]     = (uint8_t)(val >> 8);
    ram[addr + 1] = (uint8_t)(val & 0xFFu);
}

/* ------------------------------------------------------------------ */
/*  Register access                                                     */
/* ------------------------------------------------------------------ */

uint16_t agnus_read_reg(const agnus_t *ag, uint16_t offset)
{
    switch (offset) {
    case 0x002:  /* DMACONR — read mirror; bit 14 = BLTDONE (1 = idle) */
        return (uint16_t)(ag->dmacon | 0x4000u);

    case 0x004:  /* VPOSR — bit 0 = line[8] */
        return (uint16_t)((ag->line >> 8) & 1u);

    case 0x006:  /* VHPOSR — bits 15:8 = line[7:0], bits 7:0 = hpos */
        return (uint16_t)(((ag->line & 0xFFu) << 8) | (ag->hpos & 0xFFu));

    default:
        return 0;
    }
}

void agnus_write_reg(agnus_t *ag, uint16_t offset, uint16_t val)
{
    switch (offset) {

    /* ---- DMACON -------------------------------------------------- */
    case 0x096:
        if (val & 0x8000u)
            ag->dmacon |=  (uint16_t)(val & 0x7FFFu);
        else
            ag->dmacon &= (uint16_t)~(val & 0x7FFFu);
        break;

    /* ---- Blitter -------------------------------------------------- */
    case 0x040: ag->bltcon0 = val;  break;
    case 0x042: ag->bltcon1 = val;  break;
    case 0x044: ag->bltafwm = val;  break;
    case 0x046: ag->bltalwm = val;  break;

    /* BPT pointers: high word carries bits 20:16, low word bits 15:1 */
    case 0x048: ag->bltcpt = (ag->bltcpt & 0x0000FFFFu) | ((uint32_t)(val & 0x1Fu) << 16); break;
    case 0x04A: ag->bltcpt = (ag->bltcpt & 0x001F0000u) | (val & 0xFFFEu); break;
    case 0x04C: ag->bltbpt = (ag->bltbpt & 0x0000FFFFu) | ((uint32_t)(val & 0x1Fu) << 16); break;
    case 0x04E: ag->bltbpt = (ag->bltbpt & 0x001F0000u) | (val & 0xFFFEu); break;
    case 0x050: ag->bltapt = (ag->bltapt & 0x0000FFFFu) | ((uint32_t)(val & 0x1Fu) << 16); break;
    case 0x052: ag->bltapt = (ag->bltapt & 0x001F0000u) | (val & 0xFFFEu); break;
    case 0x054: ag->bltdpt = (ag->bltdpt & 0x0000FFFFu) | ((uint32_t)(val & 0x1Fu) << 16); break;
    case 0x056: ag->bltdpt = (ag->bltdpt & 0x001F0000u) | (val & 0xFFFEu); break;

    case 0x058: /* BLTSIZE — stores size; blitter_execute called by bus.c */
        break;

    case 0x060: ag->bltcmod = (int16_t)val; break;
    case 0x062: ag->bltbmod = (int16_t)val; break;
    case 0x064: ag->bltamod = (int16_t)val; break;
    case 0x066: ag->bltdmod = (int16_t)val; break;

    /* ---- Copper --------------------------------------------------- */
    case 0x080: ag->cop1lc = (ag->cop1lc & 0x0000FFFFu) | ((uint32_t)(val & 0x1Fu) << 16); break;
    case 0x082: ag->cop1lc = (ag->cop1lc & 0x001F0000u) | (val & 0xFFFEu); break;
    case 0x084: ag->cop2lc = (ag->cop2lc & 0x0000FFFFu) | ((uint32_t)(val & 0x1Fu) << 16); break;
    case 0x086: ag->cop2lc = (ag->cop2lc & 0x001F0000u) | (val & 0xFFFEu); break;

    case 0x088: /* COPJMP1 — strobe: restart from COP1LC */
        ag->copper_pc = ag->cop1lc;
        break;
    case 0x08A: /* COPJMP2 — strobe: restart from COP2LC */
        ag->copper_pc = ag->cop2lc;
        break;

    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/*  Copper                                                              */
/* ------------------------------------------------------------------ */

void agnus_copper_scanline(agnus_t *ag,
                           const uint8_t *chip_ram, uint32_t chip_ram_size,
                           agnus_write_fn write_reg)
{
    /* Copper only runs when master DMA and Copper DMA are both enabled. */
    if (!(ag->dmacon & DMACON_DMAEN) || !(ag->dmacon & DMACON_COPEN))
        return;

    for (;;) {
        if (ag->copper_pc + 3u >= chip_ram_size)
            return;

        uint16_t ir1 = ram_read16(chip_ram, ag->copper_pc,     chip_ram_size);
        uint16_t ir2 = ram_read16(chip_ram, ag->copper_pc + 2, chip_ram_size);
        ag->copper_pc += 4;

        if (ir1 & 1u) {
            /* WAIT or SKIP instruction */
            uint8_t  vp  = (uint8_t)(ir1 >> 8);
            uint8_t  hp  = (uint8_t)(ir1 & 0xFEu);
            uint8_t  bfv = (uint8_t)(ir2 >> 8);
            uint8_t  bfh = (uint8_t)(ir2 & 0xFEu);

            /* End-of-list sentinel */
            if (ir1 == 0xFFFFu && ir2 == 0xFFFEu)
                return;

            /*
             * Compare (beam & mask) >= (wait_pos & mask).
             * Treat beam as a 16-bit value: high byte = line, low byte = hpos.
             */
            uint16_t beam     = (uint16_t)((ag->line << 8) | (ag->hpos & 0xFFu));
            uint16_t wait_pos = (uint16_t)((vp << 8) | hp);
            uint16_t mask     = (uint16_t)((bfv << 8) | bfh);
            int condition = (beam & mask) >= (wait_pos & mask);

            if (ir2 & 1u) {
                /* SKIP: if condition met, skip next instruction */
                if (condition)
                    ag->copper_pc += 4;
                /* always continue executing after SKIP */
            } else {
                /* WAIT: stall until beam reaches position */
                if (!condition) {
                    ag->copper_pc -= 4; /* rewind; retry on next scanline */
                    return;
                }
                /* condition met — fall through and continue */
            }
        } else {
            /* MOVE instruction: write val to custom register at offset */
            uint16_t reg = (uint16_t)(ir1 & 0x01FEu);
            /*
             * Safety: block Copper from writing to its own jump strobes
             * (COPJMP1/COPJMP2) to prevent run-away copper execution.
             */
            if (reg != 0x088u && reg != 0x08Au)
                write_reg(reg, ir2);
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Blitter                                                             */
/* ------------------------------------------------------------------ */

/*
 * Apply an 8-bit minterm to one bit position.
 * {a,b,c} form a 3-bit index; the minterm bit at that index is the output.
 */
static uint16_t apply_minterm(uint8_t mt, uint16_t a, uint16_t b, uint16_t c)
{
    uint16_t d = 0;
    for (int bit = 0; bit < 16; bit++) {
        int idx = (((a >> bit) & 1u) << 2) |
                  (((b >> bit) & 1u) << 1) |
                  (((c >> bit) & 1u));
        if ((mt >> idx) & 1u)
            d |= (uint16_t)(1u << bit);
    }
    return d;
}

void agnus_blitter_execute(agnus_t *ag,
                           uint8_t *chip_ram, uint32_t chip_ram_size,
                           uint16_t bltsize)
{
    int height = (int)(bltsize >> 6);
    int width  = (int)(bltsize & 0x3Fu);
    if (height == 0) height = 1024;
    if (width  == 0) width  = 64;

    uint8_t  mt    = (uint8_t)(ag->bltcon0 & 0xFFu);
    int use_a = (ag->bltcon0 >> 11) & 1;
    int use_b = (ag->bltcon0 >> 10) & 1;
    int use_c = (ag->bltcon0 >>  9) & 1;
    int use_d = (ag->bltcon0 >>  8) & 1;

    /* Shift amounts */
    int ash = (ag->bltcon0 >> 12) & 0xFu;
    int bsh = (ag->bltcon1 >> 12) & 0xFu;

    uint32_t apt = ag->bltapt;
    uint32_t bpt = ag->bltbpt;
    uint32_t cpt = ag->bltcpt;
    uint32_t dpt = ag->bltdpt;

    for (int row = 0; row < height; row++) {
        uint16_t prev_a = 0;   /* A shift pipeline register */
        uint16_t prev_b = 0;   /* B shift pipeline register */

        for (int col = 0; col < width; col++) {
            uint16_t raw_a = 0, raw_b = 0, c = 0;

            if (use_a) { raw_a = ram_read16(chip_ram, apt, chip_ram_size); apt += 2; }
            if (use_b) { raw_b = ram_read16(chip_ram, bpt, chip_ram_size); bpt += 2; }
            if (use_c) { c     = ram_read16(chip_ram, cpt, chip_ram_size); cpt += 2; }

            /* Apply shift pipeline */
            uint16_t a = ash ? (uint16_t)((prev_a << (16 - ash)) | (raw_a >> ash)) : raw_a;
            uint16_t b = bsh ? (uint16_t)((prev_b << (16 - bsh)) | (raw_b >> bsh)) : raw_b;
            prev_a = raw_a;
            prev_b = raw_b;

            /* Apply first/last word masks to A */
            if (col == 0)         a &= ag->bltafwm;
            if (col == width - 1) a &= ag->bltalwm;

            uint16_t d = apply_minterm(mt, a, b, c);

            if (use_d) { ram_write16(chip_ram, dpt, chip_ram_size, d); dpt += 2; }
        }

        /* Row modulos applied after each row */
        if (use_a) apt = (uint32_t)((int32_t)apt + ag->bltamod);
        if (use_b) bpt = (uint32_t)((int32_t)bpt + ag->bltbmod);
        if (use_c) cpt = (uint32_t)((int32_t)cpt + ag->bltcmod);
        if (use_d) dpt = (uint32_t)((int32_t)dpt + ag->bltdmod);
    }

    /* Update pointers so software can read final position if needed */
    ag->bltapt = apt;
    ag->bltbpt = bpt;
    ag->bltcpt = cpt;
    ag->bltdpt = dpt;
}

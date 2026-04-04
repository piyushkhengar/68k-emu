# Plan: Amiga 500 Support — Phase 1 (Kickstart Boot Screen)

## Context

The emulator has a clean `system_t` + `mem_bus_t` abstraction used by Genesis. The CLI already accepts `--system <name>`. Adding Amiga 500 means implementing a new `src/amiga/` module and registering it — the CPU core, memory layer, and main.c need no changes.

Target: Kickstart 1.3/2.0 ROM boots, initializes, and shows the hand+disk "insert disk" screen with mouse cursor rendered via SDL2. No disk loading (Phase 2).

Usage:
```
./68k-emu --system amiga kickstart.rom
./68k-emu --system amiga --headless kickstart.rom
./68k-emu --system genesis rom.gen    # unchanged
```

---

## New Directory: `src/amiga/`

```
src/amiga/
  amiga.h / amiga.c        — system_t, PAL main loop (312 lines × 50 Hz)
  bus.h / bus.c            — mem_bus_t address decoder
  agnus.h / agnus.c        — DMA control (DMACON), Blitter, Copper engine
  denise.h / denise.c      — Bitplane fetch+render, sprites, color palette
  paula.h / paula.c        — Interrupt controller (INTENA/INTREQ/ADKCON)
  cia.h / cia.c            — CIA-A (timers, ICR, keyboard stub) + CIA-B stub
  renderer.h / renderer.c  — SDL2 window, texture, per-scanline blit
```

---

## Memory Map (`src/amiga/bus.c`)

| Address range        | Region                      | Notes |
|---------------------|-----------------------------|-------|
| 0x000000–0x07FFFF   | Chip RAM (512 KB) / **OVL** | R/W normally; **reads return ROM when `ovl_active`** (see below) |
| 0x080000–0x0FFFFF   | Chip RAM mirror / slow RAM  | On real A500 this is open bus; return 0xFF |
| 0xBFD000–0xBFDFFF   | CIA-B                       | Even bytes; reg = bits [11:8] of addr |
| 0xBFE001–0xBFEFFF   | CIA-A                       | Odd bytes; reg = bits [11:8] of addr |
| 0xC00000–0xC7FFFF   | Slow RAM (512 KB)           | A500 trapdoor expansion; zero-initialised |
| 0xDFF000–0xDFFFFF   | Custom chip registers       | Dispatch to agnus/denise/paula |
| 0xF80000–0xFFFFFF   | Kickstart ROM (512 KB)      | Read-only; writes silently ignored |

### OVL — ROM Overlay at Boot (Gary behaviour)

Gary (the Amiga's glue-logic gate array) overlays the Kickstart ROM at 0x000000 at reset so the 68000 can fetch valid reset vectors (SSP + PC) from ROM rather than uninitialised Chip RAM. Without this, `cpu_reset()` reads SSP=0, PC=0 and the CPU never executes Kickstart.

**State**: `static bool ovl_active` in `bus.c`, initialised to `true`.

**In `bus_read8/16/32`**, before the Chip RAM path:
```c
if (ovl_active && addr < 0x080000)
    return rom_read(addr & 0x7FFFF);   // mirror 512 KB ROM at 0x000000
```

**Deactivation**: CIA-A Port A (PRA) bit 0. When Kickstart writes PRA with bit 0 = 1, OVL drops:
```c
// in ciaa_write(reg=PRA, val):
if (val & 0x01) bus_set_ovl(false);
```

`bus_set_ovl()` is a one-liner in `bus.c` that clears `ovl_active`.

---

## Custom Chip Registers (`src/amiga/agnus.c`, `denise.c`, `paula.c`)

All registers are 16-bit words at offsets from 0xDFF000. Write registers with bit 15 = SET/CLR use: `if bit15 set → OR bits 14:0 in; else AND NOT bits 14:0 out`.

### Read-only status registers
| Offset | Name    | Description |
|--------|---------|-------------|
| 0x002  | DMACONR | DMA control read; bit 14 = BLTDONE (1 = blitter idle) |
| 0x004  | VPOSR   | Bit 15 = LOF (long frame), bit 0 = V8 (beam high bit) |
| 0x006  | VHPOSR  | V7–V0 in bits 15:8, H8–H1 in bits 7:0 — updated each scanline |
| 0x01C  | INTENAR | Current interrupt enable bits (read-only mirror of INTENA state) |
| 0x01E  | INTREQR | Current interrupt request bits (read-only mirror of INTREQ state) |
| 0x010  | ADKCONR | Audio/disk control read |

### Agnus — DMA + Blitter + Copper
| Offset | Name     | Write action |
|--------|----------|-------------|
| 0x098  | DMACON   | SET/CLR DMA enables: bit 9=DMAEN, bit 6=BLTEN, bit 4=COPEN, bit 3=BPLEN, bit 2=SPREN |
| 0x040  | BLTCON0  | Blitter control 0 (channel enables bits 11:8, A-shift 15:12, minterm 7:0) |
| 0x042  | BLTCON1  | Blitter control 1 (B-shift 15:12, fill/line mode 3:0) |
| 0x044  | BLTAFWM  | Blitter first word mask for A |
| 0x046  | BLTALWM  | Blitter last word mask for A |
| 0x048  | BLTCPTH  | Source C pointer high 5 bits |
| 0x04A  | BLTCPTL  | Source C pointer low 16 bits |
| 0x04C  | BLTBPTH  | Source B pointer high |
| 0x04E  | BLTBPTL  | Source B pointer low |
| 0x050  | BLTAPTH  | Source A pointer high |
| 0x052  | BLTAPTL  | Source A pointer low |
| 0x054  | BLTDPTH  | Destination pointer high |
| 0x056  | BLTDPTL  | Destination pointer low |
| 0x060  | BLTCMOD  | Source C modulo (bytes added per row) |
| 0x062  | BLTBMOD  | Source B modulo |
| 0x064  | BLTAMOD  | Source A modulo |
| 0x066  | BLTDMOD  | Destination modulo |
| 0x058  | BLTSIZE  | **Triggers blit**: bits 15:6 = height, bits 5:0 = width in words |
| 0x082  | COP1LCH  | Copper list 1 pointer high 5 bits |
| 0x084  | COP1LCL  | Copper list 1 pointer low 16 bits |
| 0x086  | COP2LCH  | Copper list 2 pointer high |
| 0x088  | COP2LCL  | Copper list 2 pointer low |
| 0x08A  | COPJMP1  | Strobe: restart Copper from COP1LC |
| 0x08C  | COPJMP2  | Strobe: restart Copper from COP2LC |
| 0x02E  | COPCON   | Copper danger bit (bit 1 = allow writes to all registers) |

### Denise — Video
| Offset | Name      | Description |
|--------|-----------|-------------|
| 0x08E  | DIWSTRT   | Display window start (V7:0 bits 15:8, H7:0 bits 7:0) |
| 0x090  | DIWSTOP   | Display window stop |
| 0x092  | DDFSTRT   | Data fetch start (H position, bits 7:3) |
| 0x094  | DDFSTOP   | Data fetch stop |
| 0x100  | BPLCON0   | Bitplane control: bits 14:12 = BPU (0–6 bitplanes), bit 0 = HAM (ignore Phase 1) |
| 0x102  | BPLCON1   | Scroll values (ignore Phase 1) |
| 0x104  | BPLCON2   | Priority control (ignore Phase 1) |
| 0x108  | BPL1MOD   | Odd bitplane modulo (bytes added after each line) |
| 0x10A  | BPL2MOD   | Even bitplane modulo |
| 0x0E0  | BPL1PTH   | Bitplane 1 pointer high; BPL2PTH–BPL6PTH at 0x0E4–0x0F0 |
| 0x0E2  | BPL1PTL   | Bitplane 1 pointer low; BPL2PTL–BPL6PTL at 0x0E6–0x0F2 |
| 0x180  | COLOR00   | Palette color 0 (12-bit RGB 0x0RGB); COLOR01–COLOR31 at 0x182–0x1BE |
| 0x120  | SPR0PTH   | Sprite 0 pointer high; SPR1–7 at 0x124–0x13E |
| 0x122  | SPR0PTL   | Sprite 0 pointer low |
| 0x140  | SPR0POS   | Sprite 0 VSTART[7:0] bits 15:8, HSTART[8:1] bits 7:0 |
| 0x142  | SPR0CTL   | VSTOP[7:0] bits 15:8, VSTART[8] bit 2, VSTOP[8] bit 1, HSTART[0] bit 0 |
| 0x144  | SPR0DATA  | Sprite 0 image data plane 1 (16 bits) |
| 0x146  | SPR0DATB  | Sprite 0 image data plane 2 |
| …      | SPR1–7    | Same pattern at offsets 0x148–0x17E |

### Paula — Interrupts
| Offset | Name   | Description |
|--------|--------|-------------|
| 0x09C  | INTENA | SET/CLR interrupt enables; bit 14=INTEN (master), bit 5=VERTB, bit 3=PORTS (CIA-A) |
| 0x09E  | INTREQ | SET/CLR interrupt requests; same bit layout as INTENA |
| 0x09A  | ADKCON | SET/CLR audio/disk control |

Interrupt level mapping (fed to `cpu_set_ipl()`):
- Level 3: VERTB (vblank), BLIT, AUD0–AUD3
- Level 2: PORTS (CIA-A interrupt line)
- Level 6: EXTER (CIA-B interrupt line)

---

## CIA Chips (`src/amiga/cia.c`)

Register index = bits [11:8] of address.

**CIA-A** (0xBFExxx, odd bytes, A12=1):

| Reg | Name | Function |
|-----|------|----------|
| 0   | PRA  | Port A: **write bit 0 = 1 → call `bus_set_ovl(false)`** (deactivates ROM overlay); reads return 0xFF |
| 1   | PRB  | Port B: return 0xFF |
| 2   | DDRA | Direction register A (ignore writes for now) |
| 3   | DDRB | Direction register B |
| 4   | TALO | Timer A counter low (read: current value; write: latch) |
| 5   | TAHI | Timer A counter high |
| 6   | TBLO | Timer B counter low |
| 7   | TBHI | Timer B counter high |
| 8   | TODLO | TOD counter low (return 0) |
| 9   | TODMID | TOD mid |
| 10  | TODHI | TOD high |
| 12  | SDR  | Serial data (keyboard byte) — return 0xFF |
| 13  | ICR  | Read: status (auto-clears); write: set interrupt mask. Bit 0=TA, bit 1=TB |
| 14  | CRA  | Timer A control: bit 0=START, bit 3=RUNMODE (one-shot), bit 4=LOAD |
| 15  | CRB  | Timer B control |

Timer A/B behavior: count down from latch at E-clock (CPU clock / 10 ≈ 709 KHz). Fire CIA interrupt (sets INTREQ PORTS bit) when timer reaches zero. For Phase 1, tick timers each scanline: subtract `scanline_cycles / 10` from counter; on underflow, set ICR status bit, call `paula_update_irq()`.

**CIA-B** (0xBFDxxx, even bytes): stub — PRB returns 0xFF (disk status: no disk), all writes ignored.

---

## Agnus — Blitter Implementation (`src/amiga/agnus.c`)

Executed **synchronously** when BLTSIZE is written (no cycle-stealing in Phase 1):

```
height = (BLTSIZE >> 6) & 0x3FF;   // 0 means 1024
width  =  BLTSIZE & 0x3F;          // 0 means 64 words
channel_en = (BLTCON0 >> 8) & 0xF; // bits: D=8, C=4, B=2, A=1 (note: USE flags)
a_shift = (BLTCON0 >> 12) & 0xF;
b_shift = (BLTCON1 >> 12) & 0xF;
minterm = BLTCON0 & 0xFF;          // 8-bit boolean: ABC terms → D

for each row in height:
    for each word in width:
        val_a = (channel_en & A_EN) ? chip_read16(bltapt) : 0xFFFF
        val_a = (val_a >> a_shift) | (prev_a << (16 - a_shift))
        apply BLTAFWM (first word) / BLTALWM (last word) mask to val_a
        val_b = (channel_en & B_EN) ? chip_read16(bltbpt) : 0xFFFF
        val_b = (val_b >> b_shift) | (prev_b << (16 - b_shift))
        val_c = (channel_en & C_EN) ? chip_read16(bltcpt) : 0
        val_d = apply_minterm(minterm, val_a, val_b, val_c)
        if (channel_en & D_EN) chip_write16(bltdpt, val_d)
        advance A/B/C/D pointers by 2
    add BLTAMOD/BLTBMOD/BLTCMOD/BLTDMOD to respective pointers

BLTDONE = 1 (idle) after completion
```

`apply_minterm(mt, a, b, c)`: for each bit position, select result using the 8 possible (a,b,c) combinations indexed into `mt`.

---

## Agnus — Copper Engine (`src/amiga/agnus.c`)

State: `copper_pc` (uint32_t), `copper_active` (bool).

**On vblank** (line 0): reset `copper_pc = COP1LC`, `copper_active = true`.

**Per scanline** (`copper_run_scanline(int line)`):
```
while copper_active:
    ir1 = chip_read16(copper_pc); copper_pc += 2
    ir2 = chip_read16(copper_pc); copper_pc += 2

    if (ir1 & 1) == 0:          // MOVE
        reg_offset = ir1 & 0x1FE
        custom_write(reg_offset, ir2)
    else:                        // WAIT or SKIP
        vp = (ir1 >> 8) & 0xFF
        hp = ir1 & 0xFE
        ve = (ir2 >> 8) | 0x80  // bit 7 of VE always 1
        he = ir2 & 0xFE
        beam_v = line & ve
        if (ir2 & 1) == 0:      // WAIT
            if (beam_v < (vp & ve)) || (beam_v == (vp & ve) && current_hpos < (hp & he)):
                copper_pc -= 4  // re-execute next scanline
                break
        else:                   // SKIP
            if beam matches: skip next instruction (copper_pc += 4)
```

Special: `ir1 == 0xFFFF && ir2 == 0xFFFE` → WAIT for end of frame, stop copper.

---

## Denise — Per-Scanline Render (`src/amiga/denise.c`)

Called once per scanline from the main loop **after** running the Copper for that line:

```
nplanes = (BPLCON0 >> 12) & 7   // 0-6 bitplanes
if nplanes == 0: fill line with COLOR00; return

// Data fetch window: DDFSTRT/DDFSTOP define which lores pixel columns have data
fetch_words = (DDFSTOP - DDFSTRT) / 8 + 1   // approx; use 20 words (320px) for Phase 1

for each fetch word (16 pixels):
    for plane in 0..nplanes-1:
        plane_word[plane] = chip_read16(bplpt[plane])
        bplpt[plane] += 2

for px in 0..319:
    word_idx = px / 16; bit_idx = 15 - (px % 16)
    color_idx = 0
    for plane in 0..nplanes-1:
        color_idx |= ((plane_word[plane][word_idx] >> bit_idx) & 1) << plane
    pixels[px] = palette[color_idx]   // 0xAARRGGBB from COLOR registers

// Sprites: for each sprite s in 0..7, check if line is in [VSTART, VSTOP)
// If so, render 16 pixels at horizontal position HSTART using COLOR17-19 (or 21-23, 25-27, 29-31)
// Sprite 0 (mouse cursor) is highest priority

// After last line in DIW: add BPL1MOD to odd planes, BPL2MOD to even planes
```

Color register decode: `COLOR[n]` stores 12-bit value 0x0RGB → expand to 0xFFRRGGBB for SDL2.

---

## SDL2 Renderer (`src/amiga/renderer.c`)

- `renderer_init()`: create SDL2 window (320×256 or 640×512 scaled ×2), create streaming texture (SDL_PIXELFORMAT_ARGB8888)
- `renderer_blit_line(int line, uint32_t *pixels)`: copy 320 pixels into texture row
- `renderer_present()`: `SDL_RenderCopy` + `SDL_RenderPresent` once per vblank; also `SDL_PollEvent` for quit/keyboard

Compiled only when `HAVE_SDL2` is defined (same pattern as Genesis renderer).

---

## Main Loop (`src/amiga/amiga.c`)

PAL timing: 7,093,180 Hz CPU / 50 Hz / 312 lines = **454 cycles per scanline**.

```c
void amiga_run(void) {
    renderer_init();
    int line = 0;
    while (!quit) {
        // 1. Run CPU for one scanline
        int budget = AMIGA_CYCLES_PER_LINE;  // 454
        while (budget > 0) {
            int c = cpu_step();
            if (!c) goto done;
            budget -= c;
        }
        // 2. Tick CIA-A/B timers
        cia_tick(AMIGA_CYCLES_PER_LINE);
        // 3. Run Copper for this scanline
        agnus_copper_run(line);
        // 4. Render this scanline (if in display window)
        denise_render_line(line);
        // 5. Advance raster
        line++;
        if (line >= AMIGA_LINES_PAL) {         // 312
            line = 0;
            // Vblank: fire VERTB interrupt (level 3)
            paula_assert_intreq(INTREQ_VERTB);
            renderer_present();
        }
    }
done:
    renderer_shutdown();
}
```

`paula_assert_intreq()` sets the bit in INTREQ and calls `paula_update_irq()` which calls `cpu_set_ipl(level)`.

---

## Modified Files

### `src/system.c`
```c
extern const system_t system_genesis;
extern const system_t system_amiga;

static const system_t *systems[] = {
    &system_genesis,
    &system_amiga,
};
```

### `Makefile`
```makefile
CFLAGS = ... -Isrc/amiga   # add include path

AMIGA_SRCS = src/amiga/bus.c src/amiga/agnus.c src/amiga/denise.c \
             src/amiga/paula.c src/amiga/cia.c src/amiga/amiga.c

SRCS = $(CORE_SRCS) $(GENESIS_SRCS) $(AMIGA_SRCS)   # default build includes amiga (headless)

# Amiga with SDL2 display:
amiga: $(filter-out src/genesis/genesis.o src/amiga/amiga.o,$(OBJS)) \
       src/genesis/genesis_sdl.o src/genesis/renderer.o src/genesis/audio.o \
       src/amiga/amiga_sdl.o src/amiga/renderer.o
    $(CC) $(CFLAGS) $(SDL2_CFLAGS) -DHAVE_SDL2 -o $(TARGET) $^ $(LDFLAGS) $(SDL2_LIBS)

src/amiga/amiga_sdl.o: src/amiga/amiga.c
    $(CC) $(CFLAGS) $(SDL2_CFLAGS) -DHAVE_SDL2 -c -o $@ $<

src/amiga/renderer.o: src/amiga/renderer.c
    $(CC) $(CFLAGS) $(SDL2_CFLAGS) -c -o $@ $<
```

---

## Implementation Order

1. `bus.c` — memory map skeleton; Chip RAM + ROM read/write; **`ovl_active` flag + `bus_set_ovl()`**; CIA/custom stubs returning 0
2. `paula.c` — INTENA/INTREQ with SET/CLR; `paula_update_irq()` → `cpu_set_ipl()`; ADKCON
3. `cia.c` — CIA-A ICR, timers A/B countdown, **PRA write → `bus_set_ovl(false)` when bit 0 set**; CIA-B stub
4. `agnus.c` — DMACON, Blitter (synchronous), Copper engine, VHPOSR/VPOSR update
5. `denise.c` — BPLCON0, DDF/DIW registers, BPLxPT, COLORxx, sprite registers; render logic
6. `renderer.c` — SDL2 window + streaming texture + per-scanline copy + present
7. `amiga.c` — `system_amiga` struct, `amiga_init/reset/shutdown/run/run_headless`
8. `src/system.c` — register `system_amiga`
9. `Makefile` — AMIGA_SRCS, amiga target, -Isrc/amiga

---

## What's Explicitly Deferred (Phase 2+)

- ADF disk loading and Paula disk DMA
- Audio DMA (Paula channels AUD0–AUD3)
- HAM / EHB / extra-halfbrite color modes
- Blitter line-draw mode (BLTCON1 bit 0)
- Full sprite DMA from memory (SPRxPT pointers) — Phase 1 reads SPRxDATA/DATB written directly by Copper
- Mouse/joystick input (JOY0DAT/JOY1DAT)
- Serial port (SERDAT/SERDATR)

---

## Verification

```bash
# 1. Build headless (no SDL2):
make
./68k-emu --system amiga --headless kickstart.rom
# Expected: runs for 120 frames without crash; no display output

# 2. Build with display:
make amiga
./68k-emu --system amiga kickstart.rom
# Expected: SDL2 window shows Kickstart boot colours progressing to
#           the hand+disk "insert disk" graphic

# 3. Genesis still works:
./68k-emu --system genesis rom.gen

# 4. Regression tests still pass:
make test
```

Kickstart diagnostic color sequence (early boot):
- Dark grey → light grey → white → Workbench blue background
- If stuck on a single color: the custom chip register corresponding to that color isn't being written (debug by printing custom_write calls)

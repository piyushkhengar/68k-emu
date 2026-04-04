# The Amiga Custom Chips: A Deep Dive

> A companion guide to the Amiga 500 emulator implementation.  
> Goal: by the time you finish reading this, you'll understand not just *what* the chips do, but *why* they were designed that way, and *how* software drives them.

---

## Table of Contents

1. [Why Custom Chips?](#1-why-custom-chips)
2. [The Big Picture: Architecture Overview](#2-the-big-picture-architecture-overview)
3. [Chip RAM: The Shared Memory Contract](#3-chip-ram-the-shared-memory-contract)
4. [Agnus: The DMA Maestro](#4-agnus-the-dma-maestro)
  - 4.1 [DMA: What It Is and Why It Matters](#41-dma-what-it-is-and-why-it-matters)
  - 4.2 [DMA Channels and Priority](#42-dma-channels-and-priority)
  - 4.3 [DMACON: The Master Switch](#43-dmacon-the-master-switch)
  - 4.4 [The Copper: A Programmable Display Coprocessor](#44-the-copper-a-programmable-display-coprocessor)
  - 4.5 [The Blitter: A Hardware Graphics Accelerator](#45-the-blitter-a-hardware-graphics-accelerator)
5. [Denise: The Artist](#5-denise-the-artist)
  - 5.1 [Bitplanes vs. Chunky Pixels](#51-bitplanes-vs-chunky-pixels)
  - 5.2 [The Display Pipeline](#52-the-display-pipeline)
  - 5.3 [Color Modes](#53-color-modes)
  - 5.4 [Hardware Sprites](#54-hardware-sprites)
6. [Paula: The Musician and Gatekeeper](#6-paula-the-musician-and-gatekeeper)
  - 6.1 [Four-Channel DMA Audio](#61-four-channel-dma-audio)
  - 6.2 [The Interrupt Controller](#62-the-interrupt-controller)
  - 6.3 [Disk and Serial](#63-disk-and-serial)
7. [The CIA Chips: The Caretakers](#7-the-cia-chips-the-caretakers)
  - 7.1 [Timers](#71-timers)
  - 7.2 [I/O Ports](#72-io-ports)
  - 7.3 [The Keyboard Protocol](#73-the-keyboard-protocol)
  - 7.4 [Time of Day Clock](#74-time-of-day-clock)
8. [Gary: The Glue](#8-gary-the-glue)
  - 8.1 [Address Decoding](#81-address-decoding)
  - 8.2 [The OVL Signal: ROM Overlay at Boot](#82-the-ovl-signal-rom-overlay-at-boot)
9. [The 68000: The Conductor](#9-the-68000-the-conductor)
  - 9.1 [Memory-Mapped I/O](#91-memory-mapped-io)
  - 9.2 [Cycle Stealing](#92-cycle-stealing)
  - 9.3 [Interrupt Acknowledgement](#93-interrupt-acknowledgement)
10. [How It All Fits Together: A Frame in the Life of an Amiga](#10-how-it-all-fits-together-a-frame-in-the-life-of-an-amiga)
11. [Register Quick Reference](#11-register-quick-reference)

---

## 1. Why Custom Chips?

In 1985, the year the Amiga 1000 launched, the most powerful home computer was arguably the Apple Macintosh — and it had no sound, no color, and its CPU did all the work of driving the display.

The Amiga's designers (led by Jay Miner, who had previously designed the Atari custom chips for the 2600 and 400/800) had a radical insight: **the CPU is the wrong tool for multimedia**. Pushing pixels, mixing audio samples, and scanning data off a floppy disk are all *repetitive memory operations* — boring work that occupies the CPU entirely when it could be running your game logic or operating system instead.

Their solution was to offload these tasks onto three dedicated custom chips: **Agnus**, **Denise**, and **Paula**. Each chip has a specific role:


| Chip       | Nickname     | Primary role                                                                                                                     |
| ---------- | ------------ | -------------------------------------------------------------------------------------------------------------------------------- |
| **Agnus**  | The Planner  | DMA controller, address generator. Owns the memory bus for most of each scanline. Houses the Copper coprocessor and the Blitter. |
| **Denise** | The Artist   | Takes the data Agnus feeds it and turns it into pixels on screen. Handles sprites.                                               |
| **Paula**  | The Musician | DMA audio (4 channels), disk I/O, serial port, and — crucially — the interrupt controller.                                       |


Two support chips, **CIA-A** and **CIA-B** (MOS 8520s, descendants of the 6526 in the C64), handle keyboards, joysticks, timers, and the power LED. A fifth chip, **Gary** (MOS 5719), acts as glue logic — it has no software-visible registers, but it decodes the address bus into chip-select signals and manages the ROM overlay at boot.

The result was a machine that, in 1985, could play four simultaneous stereo audio channels, animate 32 hardware colors at 320×200 resolution, and move sprites around the screen — all while the CPU was free to run application code. No competitor could touch it for nearly a decade.

---

## 2. The Big Picture: Architecture Overview

Here's how the chips connect:

```
                         ┌─────────────────────────────────────┐
                         │            CHIP RAM (512 KB)         │
                         │   Shared between CPU and all DMA     │
                         └────┬──────────┬───────────┬──────────┘
                              │          │           │
                         ┌────┴───┐  ┌───┴───┐  ┌───┴───┐
                         │ AGNUS  │  │DENISE │  │ PAULA │
                         │(DMA /  │  │(video │  │(audio │
                         │Copper/ │  │output)│  │/IRQ)  │
                         │Blitter)│  └───────┘  └───────┘
                         └────┬───┘
                              │ controls
                         ┌────┴──────────────────────┐
                         │        CPU (68000)          │──── address bus ────────┐
                         │   + Slow RAM (512 KB)       │                         ▼
                         │   + Kickstart ROM (512 KB)  │                  ┌──────────────┐
                         └──────────┬─────────────────┘                  │     GARY     │
                                    │                         OVL ctrl    │ (addr decode,│
                                    │                    ┌───(PRA bit 0)──│  OVL, DTACK) │
                                    │                    │                └──────┬───────┘
                                    │                    │                       │ chip-selects
                                    │                    │          ┌────────────┴────────────┐
                         ┌──────────┴──────┐             │          │                         │
                         │     CIA-A       │◄────────────┘          ▼                         ▼
                         │ (keyboard,      │◄── chip-select    (ROM, RAM,              ┌──────────┐
                         │  timers, OVL)   │    from Gary       custom regs)           │  CIA-B   │
                         └─────────────────┘                                           │  (disk,  │
                                                                                       │ parallel)│
                                                                                       └──────────┘
```

The critical thing to understand is that **Chip RAM is the center of the universe**. Every chip needs it. Agnus arbitrates who gets to use the memory bus during each clock cycle.

The CPU's address space looks like this:

```
0x000000 – 0x07FFFF   Chip RAM (512 KB) — the shared workspace
0x080000 – 0xBEFFFF   (expansion / open bus)
0xBFD000              CIA-B registers
0xBFE001              CIA-A registers
0xC00000 – 0xC7FFFF   Slow RAM (512 KB, A500 trapdoor expansion)
0xDFF000 – 0xDFFFFF   Custom chip registers (Agnus, Denise, Paula)
0xF80000 – 0xFFFFFF   Kickstart ROM (512 KB)
```

Notice that the custom chips don't have their own memory — you tell them *where in Chip RAM* to find your graphics, audio samples, and copper lists. This is the DMA model.

---

## 3. Chip RAM: The Shared Memory Contract

Chip RAM is the most important concept to understand before anything else.

On a standard computer, RAM is private to the CPU. The CPU reads and writes it freely. On the Amiga, the bottom 512 KB of RAM is **shared between the CPU and the custom chips**. Any data the custom chips need — bitplane graphics, sprites, audio samples, copper lists — must live in Chip RAM.

Why? Because the custom chips access memory directly via DMA (direct memory access) without involving the CPU. DMA requires physical access to the address and data buses, and those buses connect to Chip RAM. The "chip" in "Chip RAM" literally means "accessible to the custom chips".

**Slow RAM** (0xC00000–0xC7FFFF, the A500 trapdoor expansion) is *not* Chip RAM. It's accessible to the CPU but not to DMA. You can store code and data there, but not bitmaps or audio samples.

**The time-slicing contract**: The 68000 CPU and the custom chips share the memory bus by time-slicing it. The Amiga's memory clock runs at about 3.5 MHz (half the CPU clock of 7.09 MHz). During each memory cycle, either the CPU or the custom chips get the bus. Agnus controls this arbitration. We'll come back to this in section 8.2.

---

## 4. Agnus: The DMA Maestro

Agnus is the most complex of the three custom chips. On the Amiga 500+ it became "Fat Agnus" (capable of addressing 1 MB of Chip RAM). Its jobs:

1. **DMA controller** — schedules and executes all DMA transfers each scanline
2. **Address generator** — generates the memory addresses for bitplane and sprite fetches
3. **Houses the Copper** — a programmable coprocessor that runs from Chip RAM
4. **Houses the Blitter** — a hardware graphics accelerator

### 4.1 DMA: What It Is and Why It Matters

DMA (Direct Memory Access) means a chip accesses RAM autonomously, without the CPU fetching each byte. On the Amiga, DMA is how:

- Bitplane data gets from Chip RAM to Denise for display
- Sprite data gets from Chip RAM to Denise
- Audio samples get from Chip RAM to Paula
- The Copper program gets fetched and executed
- The Blitter reads source data and writes results

Without DMA, the CPU would have to do all of this manually. A 320×200 screen with 5 bitplanes requires fetching 320×200÷8×5 = 40,000 bytes per frame, 60 frames per second = 2.4 MB/sec of pixel data alone. At 7 MHz the 68000 can move about 3.5 MB/sec total — so pixel fetching alone would consume most of the CPU.

With DMA, the CPU does zero work for any of this. Agnus handles it all during the "DMA slots" in each scanline.

### 4.2 DMA Channels and Priority

Agnus allocates the memory bus into time slots. During each scanline, slots are given out in a fixed priority order:

```
Priority (highest to lowest):
  1. Copper          — always gets first access
  2. Blitter         — when a blit is in progress
  3. Disk            — when reading/writing a disk track
  4. Audio (AUD0–3)  — audio sample fetches
  5. Sprites         — sprite data fetches (8 sprites × 2 words)
  6. Bitplanes       — the largest consumer; BPL DMA dominates the active display area
  7. CPU             — gets whatever slots are left
```

This priority system means that heavy DMA usage (lots of bitplanes, sprites, and audio) can slow down the CPU, because fewer bus cycles are available to it. This is the famous **"CPU cycle stealing"** of the Amiga. It's not a bug — it's the designed trade-off that allows multimedia without a faster (more expensive) CPU.

### 4.3 DMACON: The Master Switch

Every DMA channel can be independently enabled or disabled via the **DMACON** register (write to 0xDFF096, read from 0xDFF002 as DMACONR).

DMACON uses a clever **SET/CLR bit** (bit 15): writing with bit 15=1 *sets* the bits you specify; writing with bit 15=0 *clears* them. This lets software atomically enable or disable individual channels without a read-modify-write cycle.

```
DMACON bit layout:
  Bit 15: SET/CLR (1 = set the specified bits, 0 = clear them)
  Bit 14: BBUSY   (read-only: 1 = blitter busy)
  Bit 10: BLTPRI  (blitter priority: 1 = blitter-nasty, steals all CPU cycles)
  Bit  9: DMAEN   (master DMA enable — must be 1 for any DMA to work)
  Bit  8: BPLEN   (bitplane DMA enable)
  Bit  7: COPEN   (copper DMA enable)
  Bit  6: BLTEN   (blitter DMA enable)
  Bit  5: SPREN   (sprite DMA enable)
  Bit  4: DSKEN   (disk DMA enable)
  Bit  3: AUD3EN  (audio channel 3 DMA)
  Bit  2: AUD2EN
  Bit  1: AUD1EN
  Bit  0: AUD0EN
```

Kickstart's very first act during boot is to save DMACONR, then write 0x7FFF to DMACON (clear all bits — disabling all DMA) while it initializes memory. This is why a working DMACON implementation is critical to booting Kickstart at all.

### 4.4 The Copper: A Programmable Display Coprocessor

The **Copper** (short for co-processor) is one of the most elegant pieces of hardware design in the Amiga. It's a simple processor that runs a program from Chip RAM, synchronized to the video beam.

**Why does this exist?** The video hardware on any display system has registers that control what's happening *right now* on screen — color palette, bitplane pointers, etc. If you want different behavior in different parts of the screen (say, a different color palette in the top half vs. the bottom half), the CPU would need to be interrupted at exact raster positions to change those registers. This burns CPU time and requires precise timing. The Copper handles this automatically, without touching the CPU.

**Instruction set**: The Copper has exactly 3 instructions, each 32 bits (two 16-bit words):

**MOVE** (bit 0 of first word = 0):

```
Word 1: destination register address (offset from 0xDFF000, must be even, ≥ 0x20)
Word 2: value to write

Example: set COLOR00 to red
  DC.W $0180, $0F00    ; MOVE $0F00 → DFF180 (COLOR00)
```

**WAIT** (bit 0 of first word = 1, bit 0 of second word = 0):

```
Word 1: VP[15:8] = vertical position, HP[7:1] = horizontal position
Word 2: VE[15:8] = vertical enable mask, HE[7:1] = horiz enable mask (bit 0 = 0)

The Copper stalls until the video beam reaches or passes (VP, HP).
VE and HE are masks — set a bit to 0 to "don't care" about that position bit.

Example: wait until the beam reaches line 100
  DC.W $6401, $FFFE    ; VP=$64 (100), HP=0, VE=$FF, HE=$FE
```

**SKIP** (bit 0 of both words = 1):

```
Same format as WAIT, but instead of halting, the Copper skips the next
instruction if the beam is already past the specified position.
Useful for conditional branches based on raster position.
```

**Copper execution**: Agnus runs the Copper continuously. At the start of each frame (vblank), it resets the Copper's program counter to **COP1LC** (Copper List 1 Location). The Copper then marches through its list, executing MOVEs immediately and pausing on WAITs until the beam catches up.

**What software does with it**: Almost every Amiga program sets up a Copper list. A typical list might:

- WAIT for line 0, then MOVE COLOR00 to set the background
- WAIT for line 44 (start of display area), MOVE to set up bitplane pointers
- WAIT for line 100, MOVE to change the color palette (split-screen effect)
- WAIT for line 256, MOVE to restore the palette
- WAIT for the end-of-frame sentinel (0xFFFF, 0xFFFE)

This gives you per-scanline control over the entire custom chip register set, with zero CPU involvement. It's why Amiga demos could have copper-bar effects, gradient skies, parallax scrolling, and palette cycling that would have been impossible on contemporary hardware.

**The two copper lists**: There are two list pointers (COP1LC and COP2LC). Most software uses only COP1LC. COP2LC allows switching to a second list mid-frame (the Copper can MOVE COPJMP2 to jump to list 2). Writing to the COPJMP1 strobe register immediately restarts the Copper from COP1LC — software does this after updating the copper list to avoid the Copper executing a partially-written list.

### 4.5 The Blitter: A Hardware Graphics Accelerator

The **Blitter** (block transfer engine) is a DMA engine that can copy, transform, and combine rectangular blocks of data in Chip RAM — without the CPU. It's the workhorse of Amiga graphics: clearing the screen, copying sprites into backgrounds, scrolling, area fills.

**Four channels**: The Blitter has four data channels — A, B, C, and D:

- **A**: primary source (typically a sprite or foreground graphic), with optional barrel shift
- **B**: secondary source (typically a mask or texture), with optional barrel shift
- **C**: tertiary source (typically the background being drawn over)
- **D**: destination (where the result is written)

A, B, and C can each be independently enabled. D is almost always enabled (otherwise you're not writing anything).

**The minterm — a 3-input boolean function unit**: The most powerful part of the Blitter is how it combines A, B, and C to produce D. Rather than offering a fixed operation ("copy A", "AND A with B", etc.), it offers a *general 3-input boolean function*.

Any 3-input boolean function can be expressed as an 8-bit truth table (2³ = 8 possible input combinations). The Blitter evaluates all 8 combinations simultaneously (one per bit position) and ORs the selected results:

```
Minterm bits (BLTCON0 bits 7:0):
  Bit 7: result when A=1, B=1, C=1  (called "ABC"  or minterm 7)
  Bit 6: result when A=1, B=1, C=0  (called "ABc"  or minterm 6)
  Bit 5: result when A=1, B=0, C=1  (called "AbC"  or minterm 5)
  Bit 4: result when A=1, B=0, C=0  (called "Abc"  or minterm 4)
  Bit 3: result when A=0, B=1, C=1  (called "aBC"  or minterm 3)
  Bit 2: result when A=0, B=1, C=0  (called "aBc"  or minterm 2)
  Bit 1: result when A=0, B=0, C=1  (called "abC"  or minterm 1)
  Bit 0: result when A=0, B=0, C=0  (called "abc"  or minterm 0)
```

Common operations encoded as minterms:


| Operation                        | Minterm               | Use case                            |
| -------------------------------- | --------------------- | ----------------------------------- |
| `D = 0` (clear)                  | `0x00`                | Clear a region of Chip RAM          |
| `D = C` (copy C)                 | `0xCA`                | Copy background unchanged           |
| `D = A` (copy A)                 | `0xF0`                | Plain copy with no masking          |
| `D = A OR C`                     | `0xFA`                | Stamp graphic onto background       |
| `D = (A AND B) OR (NOT A AND C)` | `0xCA` with B as mask | Masked blit: draw sprite using mask |
| `D = A XOR C`                    | `0x3C`                | XOR drawing (toggle pixels)         |


The masked blit (`D = (A AND B) OR (NOT_A AND C)`) is the most common. Here A is the graphic, B is its 1-bit mask (1 where graphic pixels exist), and C is the background. The result is the graphic stamped cleanly over the background. This is how all Amiga games draw sprites and objects without the rectangular bounding box showing.

**Barrel shifter**: Both A and B can be shifted left or right by 0–15 bits before the minterm logic. This allows sub-word horizontal positioning of graphics without pre-shifting data in memory — critical for smooth horizontal scrolling.

**Triggering a blit**: You set up all the parameters (pointers, modulos, control registers) and then write to **BLTSIZE** last. That single write triggers the entire operation. The blitter then proceeds autonomously while the CPU can do other work. Software checks BLTDONE (bit 14 of DMACONR) before starting a new blit.

```
; Example: clear 320×200 pixels (20 words × 200 lines) to zero
  MOVE.W  #$0000, BLTCON0    ; minterm = 0x00 (result always 0), D channel enabled
  MOVE.W  #$0000, BLTCON1    ; normal mode
  MOVE.L  #$screen, BLTDPTH  ; destination pointer (32-bit split into H/L)
  MOVE.W  #$0000, BLTDMOD    ; no modulo (contiguous)
  MOVE.W  #(200<<6)|20, BLTSIZE  ; height=200, width=20 words — this triggers the blit
```

---

## 5. Denise: The Artist

Denise receives data from Agnus (via DMA) and turns it into a video signal. She knows nothing about memory addresses — Agnus feeds her a stream of data words and she renders them as pixels.

### 5.1 Bitplanes vs. Chunky Pixels

Modern graphics hardware uses **chunky** (packed) pixel formats: each pixel is stored as a contiguous group of bits (e.g., 8 bits = 1 byte per pixel for 256 colors, or 32 bits for true color). Simple to understand, but expensive in 1985 — a 320×200 8-bit chunky screen needs 64,000 bytes.

The Amiga uses **bitplanes**: each bitplane is a single-bit-per-pixel bitmap for the entire screen. To represent 32 colors you need 5 bitplanes (2⁵ = 32). Each bitplane for a 320×200 screen is 320×200÷8 = 8,000 bytes. Five bitplanes = 40,000 bytes — half the chunky equivalent.

```
Bitplane model for 4-color (2 bitplane) display:
  Bitplane 1:  0 1 0 1 0 0 1 1 ...   (bit 0 of each pixel's color index)
  Bitplane 2:  1 1 0 0 1 0 0 1 ...   (bit 1 of each pixel's color index)
              ─────────────────
  Color index: 2 3 0 1 2 0 1 3 ...   (combined: pixel = bp2_bit<<1 | bp1_bit)
  Color:       ■ □ · ▒ ■ · □ ▒ ...   (COLOR02, COLOR03, COLOR00, COLOR01, ...)
```

Denise receives these bitplane words in parallel (one word from each active bitplane per fetch) and recombines the bits into color indices, which it looks up in the 32-entry color palette.

**Trade-off**: Bitplanes are memory-efficient but have a quirk — to change a single pixel's color requires modifying one bit in each of the bitplanes containing that pixel. This is why the Blitter's minterm logic is so important: it can update all bitplanes in one pass.

### 5.2 The Display Pipeline

Every frame, Agnus generates the raster scan from top-left to bottom-right. The pipeline:

```
Memory (Chip RAM)
    │
    │  Agnus DMA: fetches bitplane words according to BPLxPT pointers
    ▼
Denise (receives data stream)
    │
    │  Decodes bitplane bits → color index
    │  Looks up color index in COLOR00–COLOR31
    │  Composites sprites on top of playfield
    ▼
Video output (RGB signal)
```

**Key registers that define the visible area**:

- **DIWSTRT / DIWSTOP** (Display Input Window): define the top-left and bottom-right of the region where Denise outputs pixels to the video signal. Outside this window, Denise outputs the background color (COLOR00).
- **DDFSTRT / DDFSTOP** (Data Fetch Start/Stop): tell Agnus when to start and stop fetching bitplane data each scanline. This must be set slightly *before* DIWSTRT to account for the pipeline delay.

**A typical setup** for a 320-pixel-wide display:

```
DIWSTRT = $2C81  ; top-left at (129, 44) in beam coordinates
DIWSTOP = $F4C1  ; bottom-right at (449, 244)
DDFSTRT = $003C  ; start fetching at hpos 60 (slightly before DIW)
DDFSTOP = $00D4  ; stop fetching at hpos 212
BPLCON0 = $5200  ; 5 bitplanes (BPU=5), color composite video
```

**Bitplane pointers and modulos**: BPL1PT–BPL6PT (high and low 16-bit halves) point to where in Chip RAM each bitplane starts. After each line, Agnus adds **BPL1MOD** to odd-numbered bitplane pointers and **BPL2MOD** to even-numbered bitplane pointers. This allows bitplanes to be stored non-contiguously or with padding, enabling hardware vertical scrolling by adjusting the starting pointers.

### 5.3 Color Modes

OCS (Original Chip Set, as in the Amiga 500) supports several color modes:

**Standard mode**: 1–5 bitplanes = 2–32 colors from a palette of 4096 (each COLOR register is 12-bit: 4 bits red, 4 green, 4 blue).

**EHB (Extra-Half-Brite)**: 6 bitplanes, 64 colors. The upper 32 colors (color indices 32–63) are automatically half the brightness of the lower 32. Zero cost to hardware — Denise just shifts the RGB values right. Used for shadow effects.

**HAM (Hold-And-Modify)**: 6 bitplanes, up to 4096 colors on screen simultaneously. Bits 5:4 of each pixel are a mode selector:

- `00` = normal palette lookup (colors 0–15)
- `01` = modify red channel of previous pixel's color
- `10` = modify blue channel
- `11` = modify green channel

Each pixel either selects from the 16-color palette or modifies one RGB channel of the previous pixel. This "hold and modify" chain allows arbitrary colors — at the cost of horizontal color smearing artifacts if there are sharp transitions. HAM was used for photo display and video digitizer output.

### 5.4 Hardware Sprites

The Amiga has 8 hardware sprites, each 16 pixels wide and unlimited height, rendered on top of the bitplane playfield.

**Sprite data format**: Each line of a sprite consists of two 16-bit words (SprDATA and SprDATB), forming 2 bits per pixel = 4 colors. Color 0 is transparent. The actual colors come from the global color palette:

- Sprite 0 and 1 share colors COLOR17–COLOR19
- Sprite 2 and 3 share colors COLOR21–COLOR23
- Sprite 4 and 5 share colors COLOR25–COLOR27
- Sprite 6 and 7 share colors COLOR29–COLOR31

(Odd and even sprites can be "attached" to form a 4-bitplane, 16-color sprite pair.)

**Positioning**: SPRxPOS and SPRxCTL define the vertical start (VSTART) and stop (VSTOP) positions and the horizontal start (HSTART). Denise compares the current beam position against these and activates the sprite when appropriate.

**DMA fetching**: Agnus fetches sprite data from Chip RAM using SPRxPT pointers, at the right scanlines (between VSTART and VSTOP). The data is passed to Denise just-in-time for rendering.

**The mouse cursor is sprite 0**. Kickstart and Intuition position it according to mouse movement (read from CIA registers) by updating SPR0POS each vblank.

---

## 6. Paula: The Musician and Gatekeeper

Paula handles three somewhat unrelated responsibilities that share one thing: they all involve DMA and interrupts.

### 6.1 Four-Channel DMA Audio

In 1985, having any hardware audio on a personal computer was notable. Having *four-channel, 8-bit DMA audio* was extraordinary. The Mac had a single-channel beeper. The PC had a piezo buzzer.

**How it works**: Each of Paula's four audio channels (AUD0–AUD3) has:

- A pointer into Chip RAM (**AUDxPT**): where the sample data lives
- A length (**AUDxLEN**): how many 16-bit words to play
- A period (**AUDxPER**): playback speed (higher number = lower pitch)
- A volume (**AUDxVOL**): 0–64

Paula fetches sample bytes from Chip RAM autonomously via DMA, at a rate determined by AUDxPER, and converts them to an analog signal. The CPU sets up the registers and forgets about it — Paula loops through the sample buffer independently.

**At end of buffer**: Paula fires an interrupt (level 3), allowing the CPU to update AUDxPT and AUDxLEN to point at the next buffer. This "double buffering" model is how continuous audio playback works: while Paula is playing buffer A, the CPU prepares buffer B.

The four channels can be mixed to stereo: AUD0 and AUD3 go to the left channel, AUD1 and AUD2 go to the right.

**Why this matters for emulation**: Audio DMA runs at the sample rate, typically 8000–28000 Hz. We need to tick each channel's period counter at the correct rate and generate the right number of samples per frame.

### 6.2 The Interrupt Controller

Paula is the Amiga's interrupt controller. She collects interrupt signals from all the custom chips and CIAs, and presents a single interrupt level to the 68000.

**INTENA** (write, 0xDFF09C) and **INTENAR** (read, 0xDFF01C) control which interrupt sources are enabled. **INTREQ** (write, 0xDFF09E) and **INTREQR** (read, 0xDFF01E) contain pending interrupt requests.

Both registers use the same SET/CLR bit 15 convention as DMACON.

**Interrupt sources and their levels**:

```
Level 1 (lowest):
  Bit 0: TBE    — transmit buffer empty (serial port)
  Bit 1: DSKBLK — disk block finished
  Bit 2: SOFT   — software-generated interrupt

Level 2:
  Bit 3: PORTS  — CIA-A interrupt line (keyboard, timers)

Level 3:
  Bit 4: COPER  — copper (unusual, rarely used)
  Bit 5: VERTB  — vertical blank (start of new frame)
  Bit 6: BLIT   — blitter finished

Level 4:
  Bit 7: AUD0   — audio channel 0 buffer empty
  Bit 8: AUD1
  Bit 9: AUD2
  Bit 10: AUD3

Level 5:
  Bit 11: RBF   — receive buffer full (serial)
  Bit 12: DSKSYN — disk sync word matched

Level 6:
  Bit 13: EXTER  — CIA-B interrupt line (disk, parallel)

Level 7 (highest, NMI):
  Bit 14: INTEN  — master enable (set this to 1 to enable interrupts at all)
  (NMI is not normally used on A500)
```

**How it maps to the 68000**: The 68000 has interrupt priority levels 1–7 (7 = highest, non-maskable). Paula maps its interrupt sources directly onto these levels. The 68000's IPL[2:0] pins receive the current highest pending+enabled interrupt level from Paula.

The 68000's status register has a 3-bit interrupt mask (bits 10:8 of SR). The CPU only takes an interrupt if its level exceeds the current mask. Software raises the mask to 7 (via STOP or explicit SR write) to block all interrupts; lowers it to allow them.

**VERTB (bit 5, level 3)** is the most important interrupt for emulation. It fires once per frame at the start of vblank. Kickstart's ROM and all AmigaOS software synchronize their main loops to VERTB. This is when the CPU updates the copper list, moves the mouse cursor sprite, runs input handlers, etc.

**The enable dance**: When writing an interrupt handler, software must:

1. Set the bit in INTENA (enable the source)
2. Clear the bit in INTREQ first (acknowledge any stale request)
3. In the handler: clear the bit in INTREQ to acknowledge the interrupt before returning

### 6.3 Disk and Serial

**Disk**: Paula interfaces with the MFM-encoded floppy disk. This is out of scope for Phase 1, but briefly: the disk controller reads/writes raw MFM data at about 250 Kbps, triggered by DSKSYNC matching a sync word in the data stream. Paula's disk DMA then transfers decoded bytes into Chip RAM.

**Serial**: A simple UART at up to 31.25 Kbaud. SERDAT (write) and SERDATR (read) are the data registers. Out of scope for Phase 1.

---

## 7. The CIA Chips: The Caretakers

The two **CIA** chips (Complex Interface Adapter, MOS 8520) handle the slower I/O that the custom chips don't cover. They're derivatives of the famous MOS 6526 from the Commodore 64.

**CIA-A** lives at 0xBFExxx (odd-addressed bytes, accessed via byte reads/writes to odd addresses).  
**CIA-B** lives at 0xBFDxxx (even-addressed bytes).

Register access: the register index is bits [11:8] of the address. So:

- CIA-A PRA (register 0): address 0xBFE001
- CIA-A PRB (register 1): address 0xBFE101
- CIA-A ICR (register 13): address 0xBFED01

### 7.1 Timers

Each CIA has two 16-bit countdown timers, Timer A and Timer B. Each timer:

1. Is loaded with a value (written to TALOx and TAHIx — low byte first, high byte latches)
2. Counts down at the CIA's clock rate (the 68000's E-clock = CPU clock / 10 ≈ 709 KHz)
3. On underflow, fires an interrupt and optionally reloads (continuous mode) or stops (one-shot mode)

**What Kickstart uses them for**:

- CIA-A Timer A provides the system's 50 Hz (PAL) or 60 Hz (NTSC) heartbeat clock when not using vblank
- Games use timers for precise timing without consuming CPU in polling loops
- The keyboard protocol uses CIA-A's SP (serial port) input clocked by Timer A

**ICR — Interrupt Control Register** (register 13): each CIA's own interrupt mask and status register. Reading ICR returns the current interrupt status (which timer(s) fired, keyboard byte ready, etc.) and *auto-clears* it. Writing to ICR sets or clears the interrupt mask (bit 7 = set/clear, same convention as DMACON/INTENA).

When CIA-A fires any interrupt, it asserts its interrupt line, which becomes **INTREQ bit 3 (PORTS)** — a level-2 interrupt to the 68000. The interrupt handler must read CIA-A's ICR to find out *which* CIA-A source fired (timer A, timer B, serial, etc.).

Similarly, CIA-B interrupts become **INTREQ bit 13 (EXTER)** — level 6.

### 7.2 I/O Ports

Each CIA has two 8-bit I/O ports (PRA and PRB) with direction registers (DDRA and DDRB) — each bit configurable as input (0) or output (1).

**CIA-A Port A** (PRA):

- Bit 6: Power LED (0 = LED on, 1 = off) — output
- Bit 5: Disk ready signal — input  
- Bit 4–0: Various floppy control — input

**CIA-A Port B** (PRB):

- Used by the parallel port and some printer signals

**CIA-B Port A** (PRA):

- Bit 7: Disk motor control — output
- Bit 3–0: Drive select bits — output

**CIA-B Port B** (PRB):

- Parallel port data — input/output

For our emulator, CIA-A PRA returns 0xFF (no disk inserted, LED off) and CIA-B PRA/PRB are stubs. That's enough for Kickstart to proceed past the disk detection check.

### 7.3 The Keyboard Protocol

The Amiga keyboard communicates with the computer via a **serial shift protocol** using CIA-A's SP (Serial Port) register.

When a key is pressed or released, the keyboard MCU sends an 8-bit keycode as a serial stream:

- 7 bits of keycode, 1 bit for key-up/key-down (bit 0: 0=down, 1=up)
- Clocked in via CIA-A's SP pin, synchronized by CIA-A Timer A running in input-trigger mode
- When all 8 bits are received, CIA-A fires a serial interrupt (ICR bit 3 = SP)

The keycode is read from CIA-A's SP register. The computer must then acknowledge receipt by briefly toggling a handshake line — otherwise the keyboard's MCU stops sending.

For Phase 1 we return 0xFF from SP (no key), which lets Kickstart proceed normally (it just won't process any keyboard input).

### 7.4 Time of Day Clock

Each CIA has a 24-bit **TOD (Time of Day)** counter (registers TODLO, TODMID, TODHI). It increments at the power line frequency: 50 Hz (PAL) or 60 Hz (NTSC), driven by the vblank signal.

Kickstart reads TOD to implement the system clock. For our emulator, returning 0 from TOD registers is sufficient — Kickstart won't complain about the time being wrong.

---

## 8. Gary: The Glue

Gary (MOS 5719) is the Amiga 500's gate array — a chip that consolidates a lot of discrete 74-series logic that existed as separate components on the earlier Amiga 1000. Unlike Agnus, Denise, and Paula, **Gary has no software-accessible registers**. You never read or write Gary directly. It operates entirely in hardware, invisibly to any running program.

Its job is threefold: address decoding, bus control signal generation, and one critically important boot-time behaviour called the ROM overlay.

### 8.1 Address Decoding

Every time the 68000 puts an address on the bus, something has to decide which chip should respond. That's Gary. Based on the upper address bits (A17–A23), Gary generates active-low chip-select signals:


| Signal | Meaning                                          |
| ------ | ------------------------------------------------ |
| /ROME  | Select Kickstart ROM (0xF80000–0xFFFFFF)         |
| /GAME  | Select custom chip registers (0xDFF000–0xDFFFFF) |
| /RAME  | Select Chip RAM (0x000000–0x1FFFFF)              |


Gary also generates **DTACK** (data transfer acknowledge) — the handshake signal that tells the 68000 "the bus transaction is complete, you can proceed." Without DTACK, the 68000 waits indefinitely.

**For our emulator**: all of this is already implicit in the `switch` statement inside `bus_read` and `bus_write`. We route addresses to the right handler in software; real hardware does it with Gary's combinational logic. There's nothing extra to implement here.

### 8.2 The OVL Signal: ROM Overlay at Boot

This is the one Gary behaviour that *does* affect our emulator, and it's critical to understand.

**The problem**: When the 68000 comes out of reset, the very first thing it does is fetch two 32-bit values from addresses 0x000000 and 0x000004 — the initial supervisor stack pointer (SSP) and the initial program counter (PC). Whatever is at those addresses becomes the CPU's starting state.

On the Amiga, address 0x000000 is normally Chip RAM — which is uninitialised at power-on. If the CPU read uninitialised RAM as its reset vectors, it would jump to a garbage address and crash immediately.

**The solution**: Gary asserts a signal called **OVL** (overlay) at reset, which causes the Kickstart ROM to be *mirrored* at address 0x000000. So when the 68000 fetches from 0x000000–0x000007, it actually reads from ROM. The reset vectors stored at the start of Kickstart ROM point into Kickstart code, and the CPU starts executing the ROM correctly.

**Deactivating OVL**: Once Kickstart has started and has set up the exception vector table in Chip RAM, it deactivates OVL by writing to **CIA-A Port A, bit 0** (PRA bit 0). Setting that bit to 1 signals Gary to drop the overlay — from that point on, reads from 0x000000 return Chip RAM, not ROM.

```
Boot sequence with OVL:

Power-on / reset:
  Gary asserts OVL → ROM mirrored at 0x000000

68000 reset sequence:
  Reads SSP from 0x000000 → gets ROM data (valid stack pointer)
  Reads PC  from 0x000004 → gets ROM data (points into Kickstart)
  68000 starts executing Kickstart at that PC

Early in Kickstart init:
  Kickstart writes CIA-A PRA with bit 0 = 1
  Gary deactivates OVL
  0x000000 now returns Chip RAM (initially zeroed by Kickstart's memory test)
  Kickstart fills in the exception vector table in Chip RAM
```

**For our emulator**: we implement this as a single boolean flag `ovl_active`, initialised to `true`. The bus read function checks it:

```c
// In bus_read8/16/32, before the normal Chip RAM path:
if (ovl_active && addr < 0x080000) {
    // Mirror ROM: map addr into the ROM buffer (ROM is 512 KB = 0x80000 bytes)
    return rom_read(addr & 0x7FFFF);
}
```

And CIA-A's PRA write clears it:

```c
// In ciaa_write(reg=PRA, val):
if (val & 0x01) ovl_active = false;
```

Without this, `cpu_reset()` reads SSP=0 and PC=0 from uninitialised Chip RAM, the CPU immediately takes a bus error or enters an infinite loop at address 0, and Kickstart never runs.

---

## 9. The 68000: The Conductor

The Motorola 68000 is the heart of the Amiga 500. It runs at **7.09318 MHz** (PAL) or **7.15909 MHz** (NTSC). Unlike the custom chips, the 68000 doesn't *directly* drive any hardware — it orchestrates everything by reading and writing memory-mapped registers.

### 9.1 Memory-Mapped I/O

There are no special I/O instructions on the 68000 (unlike the x86's IN/OUT). All hardware interaction happens through ordinary memory reads and writes. The bus decoder routes accesses to different chips based on address ranges:

```
CPU writes to 0xDFF096  →  bus decoder sees DFF range → routes to Agnus → DMACON
CPU reads  from 0xDFF002 →  routes to Agnus → returns DMACONR
CPU writes to 0xBFE001  →  bus decoder sees BFE range → routes to CIA-A → PRA write
```

This is why the custom chip register table in our emulator is just a large `switch` in the bus read/write functions — it's exactly what the hardware does.

### 9.2 Cycle Stealing

The 68000 and custom chips share the memory bus via time-slicing. The Amiga uses a 3.58 MHz memory bus clock (half the CPU speed). Each clock cycle, either the CPU or Agnus gets the bus.

**Agnus has higher priority** than the CPU for DMA operations. During the active display area of each scanline, Agnus is constantly fetching bitplane and sprite data. These are the "stolen" cycles the CPU never gets.

The effect: during the active display area, the CPU effectively runs at 3.5 MHz instead of 7 MHz (roughly — the exact pattern depends on how many DMA channels are active). During vblank (no display output), Agnus has much less to do and the CPU gets most cycles.

**For our emulator (Phase 1)**: we don't model cycle stealing at the instruction level. We execute ~454 CPU cycles per scanline regardless. This is a simplification that works well enough for Kickstart — the timing will be slightly off from real hardware but the software will run correctly. Full cycle-accurate DMA timing is a Phase 3+ concern.

### 9.3 Interrupt Acknowledgement

When Paula asserts an interrupt level, the 68000 responds after completing its current instruction:

1. CPU saves PC and SR to the supervisor stack
2. CPU enters supervisor mode, sets the interrupt mask to the current level
3. CPU performs an **interrupt acknowledge cycle**: a special bus cycle with function code 111 (interrupt acknowledge) and the interrupt level on the address bus
4. Paula responds with the interrupt vector number (24 + level for autovectors)
5. CPU fetches the handler address from the vector table (at address `vector_number × 4`)
6. CPU jumps to the handler

For the Amiga, interrupt vectors 25–31 (autovectors for levels 1–7) live in the 68000's exception vector table in the first 1024 bytes of Chip RAM. Kickstart fills these with pointers to its own handlers early in the boot process.

For our emulator: `cpu_set_ipl(level)` in our CPU core handles this. Paula calls it whenever INTENA or INTREQ changes and the effective interrupt level changes.

---

## 10. How It All Fits Together: A Frame in the Life of an Amiga

Let's trace exactly what happens during one PAL frame (1/50th of a second, 312 scanlines):

**At vblank (scanline 0)**:

1. Paula asserts INTREQ bit 5 (VERTB) → level 3 interrupt to 68000
2. Agnus resets Copper PC to COP1LC
3. The 68000 jumps to the level-3 handler (vertical blank interrupt)
4. The VBI handler: updates mouse cursor position (reads CIA potentiometers), advances TOD counter, calls Intuition/exec scheduler, queues input events
5. The Copper starts executing its list from the beginning

**During each scanline**:

1. The Copper executes MOVEs until it hits a WAIT for a future line
2. Agnus fetches bitplane data words from Chip RAM (BPL DMA) and sends to Denise
3. Agnus fetches sprite data words and sends to Denise
4. Denise renders pixels: combines bitplane bits → color index → palette lookup → video signal
5. Paula ticks audio channel period counters; when a period expires, outputs a sample
6. CIA timers count down; if Timer A underflows, CIA-A fires interrupt → INTREQ PORTS → level-2 handler

**At end of display area (line 256 on PAL)**:

1. Bitplane DMA stops (no more pixels to display below this line)
2. CPU gets more bus cycles during the remaining ~56 lines (vertical overscan + vblank)

**CPU's perspective**: It runs its main loop in between all this. During the display period it's getting roughly half its normal cycles. During vblank it runs at nearly full speed. A typical game/application works like:

```
main_loop:
    wait for VERTB interrupt
    update game logic (AI, physics, input)
    build next frame's copper list
    set up blitter operations for background clearing/redrawing
    update audio buffer pointers
    loop
```

The custom chips handle the rest — the CPU never touches individual pixels.

---

## 11. Register Quick Reference

### Agnus (0xDFF000 base)


| Offset | R/W | Name    | Description                                           |
| ------ | --- | ------- | ----------------------------------------------------- |
| 0x002  | R   | DMACONR | DMA status; bit 14=BLTDONE                            |
| 0x004  | R   | VPOSR   | Beam vertical high + long-frame bit                   |
| 0x006  | R   | VHPOSR  | Beam V[7:0] in bits 15:8, H[8:1] in bits 7:0          |
| 0x02E  | W   | COPCON  | Copper danger bit (bit 1)                             |
| 0x040  | W   | BLTCON0 | Blitter: channel enables, A-shift, minterm            |
| 0x042  | W   | BLTCON1 | Blitter: B-shift, line/area mode                      |
| 0x044  | W   | BLTAFWM | Blitter A first word mask                             |
| 0x046  | W   | BLTALWM | Blitter A last word mask                              |
| 0x048  | W   | BLTCPTH | Blitter C source pointer high                         |
| 0x04A  | W   | BLTCPTL | Blitter C source pointer low                          |
| 0x04C  | W   | BLTBPTH | Blitter B source pointer high                         |
| 0x04E  | W   | BLTBPTL | Blitter B source pointer low                          |
| 0x050  | W   | BLTAPTH | Blitter A source pointer high                         |
| 0x052  | W   | BLTAPTL | Blitter A source pointer low                          |
| 0x054  | W   | BLTDPTH | Blitter D destination pointer high                    |
| 0x056  | W   | BLTDPTL | Blitter D destination pointer low                     |
| 0x058  | W   | BLTSIZE | **Triggers blit**: bits 15:6=height, 5:0=width(words) |
| 0x060  | W   | BLTCMOD | Blitter C modulo (bytes added per row)                |
| 0x062  | W   | BLTBMOD | Blitter B modulo                                      |
| 0x064  | W   | BLTAMOD | Blitter A modulo                                      |
| 0x066  | W   | BLTDMOD | Blitter D modulo                                      |
| 0x082  | W   | COP1LCH | Copper list 1 address high                            |
| 0x084  | W   | COP1LCL | Copper list 1 address low                             |
| 0x086  | W   | COP2LCH | Copper list 2 address high                            |
| 0x088  | W   | COP2LCL | Copper list 2 address low                             |
| 0x08A  | W   | COPJMP1 | Strobe: restart Copper from COP1LC                    |
| 0x08C  | W   | COPJMP2 | Strobe: restart Copper from COP2LC                    |
| 0x096  | W   | DMACON  | SET/CLR DMA channel enables                           |
| 0x098  | W   | (also)  | (DMACON write is at 0x096 — 0x098 is CLXCON)          |


### Denise (0xDFF000 base)


| Offset | R/W | Name     | Description                                                    |
| ------ | --- | -------- | -------------------------------------------------------------- |
| 0x08E  | W   | DIWSTRT  | Display window start (V[7:0] bits 15:8, H[7:0] bits 7:0)       |
| 0x090  | W   | DIWSTOP  | Display window stop                                            |
| 0x092  | W   | DDFSTRT  | Data fetch start horizontal position                           |
| 0x094  | W   | DDFSTOP  | Data fetch stop                                                |
| 0x0E0  | W   | BPL1PTH  | Bitplane 1 pointer high (BPL2–6 at +8 each)                    |
| 0x0E2  | W   | BPL1PTL  | Bitplane 1 pointer low                                         |
| 0x100  | W   | BPLCON0  | Bitplane control: bits 14:12=BPU (# of planes)                 |
| 0x102  | W   | BPLCON1  | Horizontal scroll                                              |
| 0x104  | W   | BPLCON2  | Priority / sprite-playfield ordering                           |
| 0x108  | W   | BPL1MOD  | Odd bitplane modulo                                            |
| 0x10A  | W   | BPL2MOD  | Even bitplane modulo                                           |
| 0x120  | W   | SPR0PTH  | Sprite 0 data pointer high (SPR1–7 at +8 each)                 |
| 0x122  | W   | SPR0PTL  | Sprite 0 data pointer low                                      |
| 0x140  | W   | SPR0POS  | Sprite 0 vertical start + horizontal start                     |
| 0x142  | W   | SPR0CTL  | Sprite 0 vertical stop + position LSBs                         |
| 0x144  | W   | SPR0DATA | Sprite 0 line data plane 1                                     |
| 0x146  | W   | SPR0DATB | Sprite 0 line data plane 2                                     |
| 0x180  | W   | COLOR00  | Palette entry 0: bits 11:8=R, 7:4=G, 3:0=B (COLOR01–31 follow) |


### Paula (0xDFF000 base)


| Offset | R/W | Name    | Description                                     |
| ------ | --- | ------- | ----------------------------------------------- |
| 0x01C  | R   | INTENAR | Interrupt enable register (read)                |
| 0x01E  | R   | INTREQR | Interrupt request register (read)               |
| 0x09A  | W   | ADKCON  | SET/CLR audio/disk control                      |
| 0x09C  | W   | INTENA  | SET/CLR interrupt enables                       |
| 0x09E  | W   | INTREQ  | SET/CLR interrupt requests                      |
| 0x0A0  | W   | AUD0LCH | Audio ch.0 sample pointer high                  |
| 0x0A2  | W   | AUD0LCL | Audio ch.0 sample pointer low                   |
| 0x0A4  | W   | AUD0LEN | Audio ch.0 sample length (words)                |
| 0x0A6  | W   | AUD0PER | Audio ch.0 period (clock ticks between samples) |
| 0x0A8  | W   | AUD0VOL | Audio ch.0 volume (0–64)                        |


### CIA-A (0xBFExxx, odd bytes, reg = addr[11:8])


| Reg | Addr     | Name   | Description                                                 |
| --- | -------- | ------ | ----------------------------------------------------------- |
| 0   | 0xBFE001 | PRA    | Port A data                                                 |
| 1   | 0xBFE101 | PRB    | Port B data                                                 |
| 2   | 0xBFE201 | DDRA   | Port A direction (1=output)                                 |
| 3   | 0xBFE301 | DDRB   | Port B direction                                            |
| 4   | 0xBFE401 | TALO   | Timer A low byte                                            |
| 5   | 0xBFE501 | TAHI   | Timer A high byte                                           |
| 6   | 0xBFE601 | TBLO   | Timer B low byte                                            |
| 7   | 0xBFE701 | TBHI   | Timer B high byte                                           |
| 8   | 0xBFE801 | TODLO  | TOD low (50/60 Hz increments)                               |
| 9   | 0xBFE901 | TODMID | TOD mid                                                     |
| 10  | 0xBFEA01 | TODHI  | TOD high                                                    |
| 12  | 0xBFEC01 | SDR    | Serial data register (keyboard)                             |
| 13  | 0xBFED01 | ICR    | Interrupt control (read=status, write=mask)                 |
| 14  | 0xBFEE01 | CRA    | Timer A control: bit0=start, bit3=one-shot, bit4=force-load |
| 15  | 0xBFEF01 | CRB    | Timer B control                                             |


---

## Further Reading

If you want to go even deeper, these are the authoritative sources:

- **Amiga Hardware Reference Manual** (3rd ed.) — the official Commodore documentation, fully available at amigadev.elowar.com. Every register, every timing diagram.
- **The AmigaOS Developer CD** — source code examples and ROM kernel manuals
- **Amiga System Programmer's Guide** (Abacus) — practical programming guide
- **"The Amiga Guru Book"** by Ralph Babel — deep OS internals

And for emulator development specifically:

- **WinUAE source code** — the reference Amiga emulator, invaluable for edge cases
- **AROS** — open-source AmigaOS clone, good for understanding OS<→hardware contracts


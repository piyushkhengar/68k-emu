# 68K CPU Emulator

A Motorola 68000 CPU emulator written in C for learning purposes. The eventual goal is to support Amiga emulation; this project focuses on mastering the 68K CPU first.

## Building

```bash
make
```

## Running

```bash
# Run built-in NOP loop (verifies CPU executes)
./68k-emu

# Run MOVE.L test (exercises MOVE.L Dn, Dn)
./68k-emu move
# or
./68k-emu test

# Run MOVEQ test (exercises MOVEQ #imm, Dn)
./68k-emu moveq

# Run ADD.L test (10 + 32 = 42)
./68k-emu add

# Run SUB.L test (50 - 8 = 42)
./68k-emu sub

# Run CMP.L test (compare 10 and 10, Z flag set)
./68k-emu cmp

# Run Bcc test (BEQ/BNE conditional branches driven by CMP)
./68k-emu bcc

# Run Bcc comprehensive test (all 14 conditions: BRA, BHI, BLS, BCC, BCS, BNE, BEQ, BVC, BPL, BMI, BGE, BLT, BGT, BLE)
./68k-emu bcc_all

# Run a ROM file (e.g. 68K test suite binary)
./68k-emu path/to/rom.bin
```

## Project Structure

```
68k-emu/
├── main.c      - Entry point, load ROM, run loop
├── cpu.c/h     - CPU state and instruction execution
├── memory.c/h  - Bus/memory interface (RAM, read/write)
├── Makefile
└── README.md
```

## Learning Roadmap

### Phase 1: Foundation (current)
- [x] CPU state (registers, SR, PC)
- [x] Memory/bus interface
- [x] Fetch-decode-execute loop
- [x] NOP, BRA.S
- [x] MOVE.L Dn, Dn (first data-moving instruction)
- [x] MOVEQ #imm, Dn (load constants, updates N/Z flags)
- [x] ADD.L Dn, Dn (updates N, Z, V, C flags)
- [x] SUB.L Dn, Dn (updates N, Z, V, C flags)
- [x] CMP.L Dn, Dn (compare, sets flags for Bcc)
- [x] Bcc (BEQ, BNE, BRA, and all 16 conditions; 8-bit and 16-bit displacement)

### Phase 2: Core Instructions
- [ ] MOVE.W, MOVE.B, MOVE immediate (full), MOVE to/from memory
- [ ] CMP variants (other sizes, addressing modes)
- [ ] ADD/SUB variants (other sizes, addressing modes)
- [ ] AND, OR, EOR, NOT
- [ ] ASL, ASR, LSL, LSR, ROL, ROR

### Phase 3: Control Flow
- [ ] BSR, RTS, RTE
- [ ] TRAP, exceptions

### Phase 4: Remaining ISA
- [ ] MULU, MULS, DIVU, DIVS
- [ ] Addressing modes
- [ ] 68K test suite (TomHarte/SingleStepTests) – pass all tests

### Phase 5: Amiga (future)
- [ ] Add custom chips (Paula, Denise, Agnus)
- [ ] Disk support (ADF)
- [ ] Display, audio

## Resources

- **M68000 Programmer's Reference Manual** – authoritative
- **TomHarte/ProcessorTests** or **SingleStepTests/m68000** – 68K test suites for emulator validation
- 68K opcode tables for instruction encoding

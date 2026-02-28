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

# Run a ROM file (e.g. Klaus Dormann's 68000 test suite)
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

### Phase 2: Core Instructions
- [ ] MOVE.W, MOVE.B, MOVE immediate (full), MOVE to/from memory
- [ ] ADD, SUB, CMP (and flag updates)
- [ ] AND, OR, EOR, NOT
- [ ] ASL, ASR, LSL, LSR, ROL, ROR

### Phase 3: Control Flow
- [ ] Bcc (branch on condition)
- [ ] BSR, RTS, RTE
- [ ] TRAP, exceptions

### Phase 4: Remaining ISA
- [ ] MULU, MULS, DIVU, DIVS
- [ ] Addressing modes
- [ ] Klaus Dormann test suite – pass all tests

### Phase 5: Amiga (future)
- [ ] Add custom chips (Paula, Denise, Agnus)
- [ ] Disk support (ADF)
- [ ] Display, audio

## Resources

- **M68000 Programmer's Reference Manual** – authoritative
- **Klaus Dormann's 68000 test suite** – verify correctness
- 68K opcode tables for instruction encoding

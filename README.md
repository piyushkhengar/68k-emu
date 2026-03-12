# 68K CPU Emulator

A Motorola 68000 CPU emulator written in C for learning purposes. The eventual goal is to support Amiga emulation; this project focuses on mastering the 68K CPU first.

## Building

```bash
make
```

## Testing

Run all regression tests (verifies nop, move, move_mem, move_w, move_b, move_w_mem, move_b_mem, move_imm, move_imm_mem, move_anp, move_disp, moveq, add, sub, cmp, bcc, bcc_all, bsr_rts, lea, jmp, jsr, tst, clr):

```bash
make test
```

## Running

```bash
# Run built-in NOP loop (verifies CPU executes)
./68k-emu

# Run MOVE.L test (exercises MOVE.L Dn, Dn)
./68k-emu move
# or
./68k-emu test

# Run MOVE.L memory test (store/load via (A7))
./68k-emu move_mem

# Run MOVE.W test (16-bit register copy)
./68k-emu move_w

# Run MOVE.B test (8-bit register copy)
./68k-emu move_b

# Run MOVE.W / MOVE.B memory tests (store/load via (A7))
./68k-emu move_w_mem
./68k-emu move_b_mem

# Run MOVE immediate tests
./68k-emu move_imm
./68k-emu move_imm_mem

# Run MOVE (An)+ and d(An) tests
./68k-emu move_anp
./68k-emu move_disp

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

# Run Bcc comprehensive test (all 15 conditions: BRA, BSR, BHI, BLS, BCC, BCS, BNE, BEQ, BVC, BPL, BMI, BGE, BLT, BGT, BLE)
./68k-emu bcc_all

# Run BSR/RTS test (subroutine call and return)
./68k-emu bsr_rts

# Run LEA, JMP, JSR, TST, CLR tests
./68k-emu lea
./68k-emu jmp
./68k-emu jsr
./68k-emu tst
./68k-emu clr

# Run a ROM file (e.g. 68K test suite binary)
./68k-emu path/to/rom.bin

# Run MicroCore Labs 68K opcode test (after building - see below)
./68k-emu mcl68_test.bin
```

## Building the MicroCore Labs Test ROM

The [MicroCore Labs MC68000 test](https://github.com/MicroCoreLabs/Projects/tree/master/MCL68/MC68000_Test_Code) exercises all 68000 opcodes. To build and run:

1. **Install vasm** (68K Motorola syntax assembler):
   ```bash
   git clone https://github.com/dbuchwald/vasm /tmp/vasm-src
   cd /tmp/vasm-src && make CPU=m68k SYNTAX=mot
   ```

2. **Download and prepare the source** (fixes Easy68K→vasm syntax):
   ```bash
   curl -sL "https://raw.githubusercontent.com/MicroCoreLabs/Projects/master/MCL68/MC68000_Test_Code/MC68000_test_all_opcodes.X68" -o MC68000_test_all_opcodes.X68
   python3 -c "import re; c=open('MC68000_test_all_opcodes.X68').read(); c=re.sub(r'\s*,\s*', ',', c); open('MC68000_test_all_opcodes_fixed.X68','w').write(c)"
   # Replace SIMHALT (Easy68K) with: bra *
   ```

3. **Assemble**:
   ```bash
   /tmp/vasm-src/vasmm68k_mot -m68000 -Fbin -o mcl68_test.bin MC68000_test_all_opcodes_fixed.X68
   ./68k-emu mcl68_test.bin
   ```

## Project Structure

```
68k-emu/
├── Makefile
├── README.md
└── src/
    ├── main.c              - Entry point, load ROM, run loop
    ├── timing.c/h          - Cycle counts
    ├── tests.c/h           - Built-in test ROMs and harness
    ├── core/               - CPU core, memory, effective address
    │   ├── cpu.c/h         - CPU state, fetch, flags, dispatch table
    │   ├── cpu_internal.h  - Shared declarations for instruction modules
    │   ├── memory.c/h      - Bus/memory interface (RAM, read/write)
    │   └── ea.c/h          - Effective address resolution
    └── isa/                - Instruction implementations
        ├── alu.c/h         - MOVEQ, ADD, SUB, CMP, ADDA, SUBA, CMPA, ADDX, SUBX
        ├── move.c/h        - MOVE.B, MOVE.W, MOVE.L handlers
        ├── branch.c/h      - Bcc (branch on condition) handlers
        ├── control.c/h     - NOP, RTS, RTE, TRAP, NOT handlers
        ├── immediate.c/h  - ADDI, SUBI, CMPI, ADDQ, SUBQ
        ├── logic.c/h       - AND, OR, EOR handlers
        └── shift.c/h       - ASL, ASR, LSL, LSR, ROL, ROR, ROXL, ROXR
```

## Learning Roadmap

Phases 1–4 complete. Phase 5 in progress.

### Phase 1: Foundation
- [x] CPU state (registers, SR, PC)
- [x] Memory/bus interface
- [x] Fetch-decode-execute loop
- [x] NOP, BRA.S
- [x] MOVE.L Dn, Dn (first data-moving instruction)
- [x] MOVE.L (An), Dn and MOVE.L Dn, (An) (memory load/store)
- [x] MOVE.W, MOVE.B (Dn,Dn; (An),Dn; Dn,(An))
- [x] MOVE #imm (to Dn, (An), d(An))
- [x] MOVE (An)+, -(An), d(An) addressing modes
- [x] MOVEQ #imm, Dn (load constants, updates N/Z flags)
- [x] ADD.L Dn, Dn (updates N, Z, V, C flags)
- [x] SUB.L Dn, Dn (updates N, Z, V, C flags)
- [x] CMP.L Dn, Dn (compare, sets flags for Bcc)
- [x] Bcc (BEQ, BNE, BRA, BSR, and all 16 conditions; 8-bit and 16-bit displacement)
- [x] BSR (branch to subroutine), RTS (return from subroutine)

### Phase 2: Core Instructions
- [x] CMP variants (other sizes, addressing modes)
- [x] ADD/SUB variants (other sizes, addressing modes)
- [x] AND, OR, EOR, NOT
- [x] ASL, ASR, LSL, LSR, ROL, ROR, ROXL, ROXR

### Phase 3: Control Flow
- [x] RTE (return from exception)
- [x] TRAP, exceptions

### Phase 4: Jumps & Utilities
- [x] LEA (Load Effective Address)
- [x] JMP (Jump)
- [x] JSR (Jump to Subroutine)
- [x] TST (Test)
- [x] CLR (Clear)

### Phase 5: Remaining ISA (current)
- [x] MULU, MULS, DIVU, DIVS
- [x] Addressing modes
- [ ] 68K test suite (TomHarte/SingleStepTests) – pass all tests

### Phase 6: Extended ISA
- [x] EXG, ABCD, SBCD
- [x] STOP, TRAPV, CHK, RESET
- [x] MOVE to CCR, MOVE to SR
- [x] PEA (Push Effective Address)
- [x] NBCD (Negate BCD)
- [x] ROL/ROR/ROXL/ROXR memory form
- [x] Line 1010 (0xAxxx) vector 10 handler

### Phase 7: Amiga (future)
- [ ] Add custom chips (Paula, Denise, Agnus)
- [ ] Disk support (ADF)
- [ ] Display, audio

## Resources

- **M68000 Programmer's Reference Manual** – authoritative
- **TomHarte/ProcessorTests** or **SingleStepTests/m68000** – 68K test suites for emulator validation
- 68K opcode tables for instruction encoding

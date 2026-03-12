# Plan: Drive ProcessorTests Failures Toward Zero

**Goal:** Minimize processor test failures through targeted fixes.

---

## Current State (from processor_test_results_all.txt)

*Note: Run `make processor-tests` and capture per-file results for up-to-date counts. The shift dispatch fix (Phase 12) improved ASL/ASR/LSL/LSR; EXT.w improved to ~8031 pass.*

---

## Priority 1: Highest-Impact Fixes (8000+ failures each)

| Instruction | Failed | Root Cause Hypothesis |
|-------------|--------|------------------------|
| **RTE** | 8011 | Stack format (format word vs no format word for 68000) |
| **RTR** | 7539 | Stack layout: CCR then PC; pop order |
| **EORItoSR** | 8038 | Privilege (already added S=1 for ORI/ANDI; verify EORI) |
| **EXT.w** | ~34 | SR flags (Z/N) on edge cases – mostly fixed |
| **MOVEtoSR** | 3127 | SR mask 0xA7/0x1F – **DONE** (4938 pass); remaining ssp delta |
| **MOVEA.l/w** | ~~8017/8024~~ | CC not affected – **DONE** (1353 pass); remaining ssp/cascade |
| **ASL/LSL.b/l** | 8000+ | Shift size/count extraction (bits 7-6 vs 8-7) |

**Actions:**
1. **RTE/RTR** – Compare stack layout with 68000 PRM; run `PROCESSOR_TEST_INDEX=0 make processor-tests PROC_FILTER=RTE` and inspect JSON.
2. **EORItoSR** – Add 0x0A7C to the privilege override in `apply_initial()` (may already be there).
3. **MOVEA** – Check sign-extension of source for MOVEA.W/L.
4. **Shift bits** – ~~Fix `shift_size()` to use `(op >> 7) & 3`~~ **Resolved:** bits 7-6 are correct; see SHIFT_OPCODE_INVESTIGATION.md.

---

## Priority 2: Control Flow (4000–6000 failures each)

| Instruction | Failed | Root Cause |
|-------------|--------|------------|
| **JMP** | 5045 | PC-relative EA base; extension word consumption |
| **JSR** | 5029 | Same + stack push |
| **BSR** | 3995 | Same |
| **RTS** | 4057 | Correct per spec – **verified** (4008 pass); remaining ssp delta |
| **Bcc** | 4164 | 16-bit disp base = extension addr – **DONE** (11966 pass); remaining ssp |

**Actions:** Verify PC used for `(d16,PC)` is PC *after* opcode+extension; check instruction length accounting.

---

## Priority 3: Memory/EA and Size (3000–6000 failures each)

| Instruction | Failed | Root Cause |
|-------------|--------|------------|
| **MOVE.b/w/l** | 7090/7666/7664 | SSP deltas (0x7F2 vs 0x7FA); (An)+/-(An) step; extension word order |
| **MOVEM** | 5322 pass, 10808 fail | Same SSP pattern; `-(An)`/`(An)+` order |
| **CMP/ADD/SUB** | CMP: 27400 pass (was 13704) | CMP/CMPI/CMPA: X not affected – **DONE**; ADD unchanged |
| **CLR/NOT/NEG** (w/l) | 3000+ | Size; EA modes |

---

## Priority 4: Shift/Rotate (7000+ total)

| Instruction | Failed | Root Cause |
|-------------|--------|------------|
| **ASL/LSL/ASR/LSR** (b/l) | 8000+ | Size bits; register count |
| **ROL/ROR/ROXL/ROXR** | 7500+ | Same |

**Action:** Fix dispatch (done); fix size/count bit extraction in `shift.c`.

---

## Implementation Order

```mermaid
flowchart TD
    P1[P1: RTE/RTR stack format] --> P2[P2: EORItoSR privilege]
    P2 --> P3[P3: Shift size/count bits]
    P3 --> P4[P4: MOVEA sign-extension]
    P4 --> P5[P5: JMP/JSR/BSR/RTS PC]
    P5 --> P6[P6: MOVE/MOVEM size/EA]
```

---

## Verification

After each fix:

```bash
make processor-tests PROC_FILTER=InstructionName
```

For full run:

```bash
make processor-tests 2>&1 | tee results.txt
```

---

## Quick Wins Checklist

- [x] `apply_initial()`: Ensure 0x0A7C (EORI to SR) in privilege override – **DONE**
- [x] `control.c`: EXT.w – N/Z flags must reflect sign-extended word, not full 32-bit – **DONE** (8065 pass)
- [x] `control.c`: RTR – CCR restore: only lower 5 bits (X,N,Z,V,C); `(ccr & 0x1F)` – **DONE** (4038 pass)
- [ ] `shift.c`: size/count bit extraction – reverted (bits 8-7 caused regressions; needs opcode-specific decode)
- [x] `control.c`: RTE – SR mask 0xA7 (high byte) + 0x1F (CCR); only implemented bits restored – **DONE** (4011 pass)
- [x] `control.c`: MOVEtoSR – same SR mask 0xA7/0x1F – **DONE** (4938 pass)
- [x] `control.c`: MOVEtoCCR – CCR mask 0x1F (only X,N,Z,V,C) – **DONE** (4958 pass)
- [x] `branch.c` + `immediate.c`: Bcc/DBcc 16-bit disp – base = extension word addr; target = (PC-2)+disp – **DONE**
- [x] BSR/RTS – verified correct per 68k.hax.com; remaining failures = ssp test-data delta
- [x] `shift.c`: size bits 7-6 confirmed correct; (op>>7)&3 causes regression – see SHIFT_OPCODE_INVESTIGATION.md
- [x] `move.c`: MOVEA – skip set_nz_from_val when dst is An (CC not affected); sign-extend already correct in ea_store_value – **DONE** (1353 MOVEA pass, 681 MOVEA.w)

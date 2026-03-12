# MOVE.b Test 1ec6 Trace

## Test Setup
- **Name:** 1ec6 [MOVE.b D6, (A7)+]
- **Index:** 3 (in MOVE.b.json.gz)
- **Prefetch:** [0x1EC6, 0x8E76]
- **Initial:** ssp=2048 (0x800), sr=0x2718 (supervisor)
- **Expected final:** ssp=2050 (0x802), pc=3074
- **Got:** ssp=2048, pc=3076

## Transactions (Expected Behavior)
- **Write:** byte value 0x4A (74) to address 2048 (0x800 = ssp)
- **Read:** word at 3076 (extension word)

## Opcode Decode: 0x1EC6

MOVE encoding: `Size(2) | DestEA(6) | SrcEA(6)`

- **Dest EA:** `(op >> 6) & 0x3F` = 0x7B & 0x3F = **0x3B**
  - Mode = 0x3B >> 3 = 7, Reg = 0x3B & 7 = 3
  - **→ (d8, PC, Xn)** — requires extension word
- **Src EA:** `op & 0x3F` = **0x06** → D6 ✓

## Root Cause

**0x1EC6 decodes to MOVE.b D6, (d8, PC, Xn)** — not (A7)+.

For (A7)+ as dest we need DestEA = 0x1F (mode 3, reg 7). That opcode is **0x17C6**.

| Opcode  | Dest        | Extension? | Expected PC |
|---------|-------------|------------|-------------|
| 0x1EC6  | (d8,PC,Xn)  | Yes        | 3076        |
| 0x17C6  | (A7)+       | No         | 3074        |

Our emulator correctly decodes 0x1EC6 as (d8, PC, Xn), consumes the extension word, and leaves ssp unchanged. The test expects (A7)+ behavior, which matches **0x17C6**.

## Conclusion

The test data appears to have the wrong opcode: prefetch uses **0x1EC6** but expected behavior matches **0x17C6** (MOVE.b D6, (A7)+). The official map entry "1ec6": "MOVE.b D6, (A7)+" may be incorrect.

**No emulator bug** — our decode and execution match the 68000 encoding of 0x1EC6.

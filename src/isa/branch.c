#include "cpu_internal.h"
#include "branch.h"
#include "timing.h"

/* Bcc condition codes: return true if condition met. 0=BRA,1=BSR,2=BHI,3=BLS,4=BCC,5=BCS,6=BNE,7=BEQ, etc. */
int branch_condition_met(uint8_t cond)
{
    uint8_t negative_flag = (cpu.sr & SR_N) ? 1 : 0;
    uint8_t zero_flag = (cpu.sr & SR_Z) ? 1 : 0;
    uint8_t overflow_flag = (cpu.sr & SR_V) ? 1 : 0;
    uint8_t carry_flag = (cpu.sr & SR_C) ? 1 : 0;

    switch (cond) {
        case 0x0: return 1;   /* T: always true */
        case 0x1: return 0;   /* F: always false (DBcc DBF/DBRA; BSR handled separately) */
        case 0x2: return !carry_flag && !zero_flag;
        case 0x3: return carry_flag || zero_flag;
        case 0x4: return !carry_flag;
        case 0x5: return carry_flag;
        case 0x6: return !zero_flag;
        case 0x7: return zero_flag;
        case 0x8: return !overflow_flag;
        case 0x9: return overflow_flag;
        case 0xA: return !negative_flag;
        case 0xB: return negative_flag;
        case 0xC: return (negative_flag && overflow_flag) || (!negative_flag && !overflow_flag);
        case 0xD: return (negative_flag && !overflow_flag) || (!negative_flag && overflow_flag);
        case 0xE: return (negative_flag && overflow_flag && !zero_flag) || (!negative_flag && !overflow_flag && !zero_flag);
        case 0xF: return zero_flag || (negative_flag && !overflow_flag) || (!negative_flag && overflow_flag);
        default: return 0;
    }
}

/* Bcc / BSR: branch on condition (0x6xxx).  cond in bits 11-8.
 *
 * Three displacement encodings share the same opcode:
 *   low byte != 0 and != 0xFF  — 8-bit signed displacement in the low byte
 *   low byte == 0              — fetch a 16-bit signed displacement word
 *   low byte == 0xFF           — 68020+ only: fetch a 32-bit signed displacement long
 *
 * In all cases the displacement is measured from opcode_addr + 2.  After we
 * fetch the extra word(s) the PC has advanced further, so we subtract:
 *   adj = 0  for 8-bit  (PC is already at opcode+2, nothing extra fetched)
 *   adj = 2  for 16-bit (PC advanced 2 bytes past the displacement word)
 *   adj = 4  for 32-bit (PC advanced 4 bytes past the displacement long)
 *
 * On 68000/68010, low byte 0xFF is treated as a signed -1 displacement, which
 * lands on an odd address and triggers an address error — same as before. */
int op_bcc(uint16_t op)
{
    uint8_t cond = (op >> 8) & 0x0F;
    int32_t disp;
    int adj = 0;

    if ((op & 0xFF) == 0xFF && cpu.features.has_full_ea) {
        /* 68020+ 32-bit displacement */
        disp = (int32_t)fetch32();
        adj  = 4;
    } else if ((op & 0xFF) != 0) {
        /* 8-bit displacement */
        disp = (int8_t)(op & 0xFF);
    } else {
        /* 16-bit displacement */
        disp = (int16_t)fetch16();
        adj  = 2;
    }

    if (cond == 0x1) {
        /* BSR: push return address, then jump. */
        uint32_t sp = cpu_sp() - 4;
        mem_write32(sp, cpu.pc);
        cpu_sp_set(sp);
        pending_cycles += 10;
        cpu.pc += disp - adj;
        if (cpu.pc & 1)
            cpu_take_addr_err(cpu.pc, op);
        return CYCLES_BSR;
    }
    {
        int taken = branch_condition_met(cond);
        if (taken) {
            uint32_t from = cpu.pc;
            cpu.pc += disp - adj;
            cpu_trace_branch_to(from, cpu.pc);
            if (cpu.pc & 1) {
                pending_cycles += 2;
                cpu_take_addr_err(cpu.pc, op);
            }
        }
        return taken ? CYCLES_BCC_TAKEN : (adj == 0 ? CYCLES_BCC_NOT : CYCLES_BCC_WORD_NOT);
    }
}

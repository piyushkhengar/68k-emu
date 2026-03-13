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

/* Bcc: branch on condition. 0x6xxx, cond in bits 11-8. 8-bit disp in low byte; if 0, fetch 16-bit. BSR pushes return addr.
 * Base PC for displacement: 8-bit = addr of opcode+2 (PC on entry); 16-bit = addr of extension word (PC before fetch16).
 * After fetch16() for 16-bit, PC points past extension; target = (PC-2) + disp. */
int op_bcc(uint16_t op)
{
    uint8_t cond = (op >> 8) & 0x0F;
    int32_t disp;
    int is_16bit = 0;

    if ((op & 0xFF) != 0) {
        disp = (int8_t)(op & 0xFF);
    } else {
        disp = (int16_t)fetch16();
        is_16bit = 1;
    }

    if (cond == 0x1) {
        uint32_t sp = cpu_sp() - 4;
        mem_write32(sp, cpu.pc);
        cpu_sp_set(sp);
        cpu.pc += disp - (is_16bit ? 2 : 0);
        return CYCLES_BSR;
    }
    {
        int taken = branch_condition_met(cond);
        if (taken) {
            uint32_t from = cpu.pc;
            cpu.pc += disp - (is_16bit ? 2 : 0);
            cpu_trace_branch_to(from, cpu.pc);
        }
        return taken ? CYCLES_BCC_TAKEN : CYCLES_BCC_NOT;
    }
}

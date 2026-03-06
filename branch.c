#include "cpu_internal.h"
#include "branch.h"

static int bcc_condition_met(uint8_t cond)
{
    uint8_t n = (cpu.sr & SR_N) ? 1 : 0;
    uint8_t z = (cpu.sr & SR_Z) ? 1 : 0;
    uint8_t v = (cpu.sr & SR_V) ? 1 : 0;
    uint8_t c = (cpu.sr & SR_C) ? 1 : 0;

    switch (cond) {
        case 0x0: return 1;
        case 0x1: return 1;
        case 0x2: return !c && !z;
        case 0x3: return c || z;
        case 0x4: return !c;
        case 0x5: return c;
        case 0x6: return !z;
        case 0x7: return z;
        case 0x8: return !v;
        case 0x9: return v;
        case 0xA: return !n;
        case 0xB: return n;
        case 0xC: return (n && v) || (!n && !v);
        case 0xD: return (n && !v) || (!n && v);
        case 0xE: return (n && v && !z) || (!n && !v && !z);
        case 0xF: return z || (n && !v) || (!n && v);
        default: return 0;
    }
}

void op_bcc(uint16_t op)
{
    uint8_t cond = (op >> 8) & 0x0F;
    int32_t disp;

    if ((op & 0xFF) != 0) {
        disp = (int8_t)(op & 0xFF);
    } else {
        disp = (int16_t)fetch16();
    }

    if (cond == 0x1) {
        cpu.a[7] -= 4;
        mem_write32(cpu.a[7], cpu.pc);
        cpu.pc += disp;
    } else if (bcc_condition_met(cond)) {
        cpu.pc += disp;
    }
}

#include "cpu_internal.h"
#include "ea.h"
#include "memory.h"

/* 68K stack (A7) keeps word alignment: byte (An)+/-(An) use 2 for A7. */
int ea_step(int reg, int size)
{
    return (reg == 7 && size == 1) ? 2 : size;
}

static uint32_t mem_read_sized(uint32_t addr, int size)
{
    if (size == 1) return mem_read8(addr) & 0xFF;
    if (size == 2) return mem_read16(addr) & 0xFFFF;
    return mem_read32(addr);
}

static void mem_write_sized(uint32_t addr, int size, uint32_t value)
{
    if (size == 1) mem_write8(addr, (uint8_t)(value & 0xFF));
    else if (size == 2) mem_write16(addr, (uint16_t)(value & 0xFFFF));
    else mem_write32(addr, value);
}

static uint32_t decode_indexed_addr(uint32_t base)
{
    uint16_t ext = fetch16();
    int32_t disp = (int8_t)(ext >> 8);
    int idx_reg = ext & 0x0F;
    int idx_is_addr = (ext >> 6) & 1;
    int idx_long = (ext >> 4) & 1;
    uint32_t idx_val = idx_is_addr ? cpu.a[idx_reg] : cpu.d[idx_reg];
    if (!idx_long)
        idx_val = (uint32_t)(int32_t)(int16_t)(idx_val & 0xFFFF);
    return base + disp + idx_val;
}

/* Returns 1 if addr was resolved (memory EA), 0 otherwise. */
static int ea_resolve_addr(int mode, int reg, int size, uint32_t *addr)
{
    switch (mode) {
    case 0: /* Dn */
    case 1: /* An */
        return 0;
    case 2: /* (An) */
        *addr = cpu.a[reg];
        return 1;
    case 3: /* (An)+ */
        *addr = cpu.a[reg];
        cpu.a[reg] += ea_step(reg, size);
        return 1;
    case 4: /* -(An) */
        cpu.a[reg] -= ea_step(reg, size);
        *addr = cpu.a[reg];
        return 1;
    case 5: /* d(An) */
        *addr = cpu.a[reg] + (int32_t)(int16_t)fetch16();
        return 1;
    case 6: /* (d8,An,Xn) */
        *addr = decode_indexed_addr(cpu.a[reg]);
        return 1;
    case 7:
        switch (reg) {
        case 0: /* abs.w */
            *addr = (int32_t)(int16_t)fetch16();
            return 1;
        case 1: /* abs.l */
            *addr = fetch32();
            return 1;
        case 2: /* d(PC) */
            *addr = cpu.pc + (int32_t)(int16_t)fetch16();
            return 1;
        case 3: /* (d8,PC,Xn) */
            *addr = decode_indexed_addr(cpu.pc);
            return 1;
        case 4: /* #imm */
            return 0;
        default:
            return 0;
        }
    default:
        return 0;
    }
}

uint32_t ea_fetch_value(int mode, int reg, int size)
{
    uint32_t addr;

    if (ea_resolve_addr(mode, reg, size, &addr))
        return mem_read_sized(addr, size);

    switch (mode) {
    case 0: /* Dn */
        return cpu.d[reg];
    case 1: /* An */
        return cpu.a[reg];
    case 7:
        if (reg == 4) { /* #imm */
            if (size == 1) return fetch16() & 0xFF;
            if (size == 2) return fetch16() & 0xFFFF;
            return fetch32();
        }
        return 0;
    default:
        return 0;
    }
}

void ea_store_value(int mode, int reg, int size, uint32_t value)
{
    uint32_t addr;

    if (ea_resolve_addr(mode, reg, size, &addr)) {
        mem_write_sized(addr, size, value);
        return;
    }

    switch (mode) {
    case 0: /* Dn - MOVE zero-extends byte/word to long */
        if (size == 1) cpu.d[reg] = value & 0xFF;
        else if (size == 2) cpu.d[reg] = value & 0xFFFF;
        else cpu.d[reg] = value;
        break;
    case 1: /* An */
        if (size == 2) cpu.a[reg] = (uint32_t)(int32_t)(int16_t)(value & 0xFFFF);  /* sign-extend word */
        else cpu.a[reg] = value;
        break;
    default:
        break;
    }
}

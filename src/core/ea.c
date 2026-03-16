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

/* 68K brief extension word for (d8,An,Xn). Per Musashi/M68000:
 * Bits 15-12: D/A (bit 15) + register (14-12). Bits 11-9: W/L (11) + scale (10-9).
 * Bit 8: 0 (brief). Bits 7-0: 8-bit displacement (sign-extended). */
static uint32_t decode_indexed_addr(uint32_t base)
{
    uint16_t ext = fetch16();
    int32_t disp = (int8_t)(ext & 0xFF);
    int idx_reg = (ext >> 12) & 7;
    int idx_is_addr = (ext >> 15) & 1;
    int idx_long = (ext >> 11) & 1;
    uint32_t idx_val = idx_is_addr ? cpu.a[idx_reg] : cpu.d[idx_reg];
    if (!idx_long)
        idx_val = (uint32_t)(int32_t)(int16_t)(idx_val & 0xFFFF);
    /* 68000: full 32-bit address calculation; 24-bit mask for memory access is in mem_* */
    return base + disp + idx_val;
}

/* Returns 1 if addr was resolved (memory EA), 0 otherwise. */
int ea_resolve_addr(int mode, int reg, int size, uint32_t *addr)
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
        if (reg == 7) {
            if (cpu.sr & 0x2000) cpu.ssp = cpu.a[7];
            else cpu.usp = cpu.a[7];
        }
        return 1;
    case 4: /* -(An) */
        cpu.a[reg] -= ea_step(reg, size);
        *addr = cpu.a[reg];
        if (reg == 7) {
            if (cpu.sr & 0x2000) cpu.ssp = cpu.a[7];
            else cpu.usp = cpu.a[7];
        }
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

/* Public: compute EA to address only (for LEA, JMP, JSR, PEA). Uses size 4 for (An)+/-(An). */
int ea_address_no_fetch(int mode, int reg, uint32_t *addr_out)
{
    if (mode == 0 || mode == 1)
        return 0;
    if (mode == 7 && reg == 4)
        return 0;
    return ea_resolve_addr(mode, reg, 4, addr_out);
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

uint32_t ea_read_rmw(int mode, int reg, int size, ea_rmw_t *rmw)
{
    rmw->mode   = mode;
    rmw->reg    = reg;
    rmw->size   = size;
    rmw->is_mem = ea_resolve_addr(mode, reg, size, &rmw->addr);
    if (rmw->is_mem)
        return mem_read_sized(rmw->addr, size);
    /* Register EA: Dn (mode 0) or An (mode 1). */
    if (mode == 0) return cpu.d[reg];
    if (mode == 1) return cpu.a[reg];
    return 0;
}

void ea_write_rmw(const ea_rmw_t *rmw, uint32_t value)
{
    if (rmw->is_mem) {
        mem_write_sized(rmw->addr, rmw->size, value);
        return;
    }
    switch (rmw->mode) {
    case 0: /* Dn: preserve upper bits for byte/word */
        if (rmw->size == 1)      cpu.d[rmw->reg] = (cpu.d[rmw->reg] & 0xFFFFFF00) | (value & 0xFF);
        else if (rmw->size == 2) cpu.d[rmw->reg] = (cpu.d[rmw->reg] & 0xFFFF0000) | (value & 0xFFFF);
        else                     cpu.d[rmw->reg] = value;
        break;
    case 1: /* An: word result is sign-extended to 32 bits */
        if (rmw->size == 2) cpu.a[rmw->reg] = (uint32_t)(int32_t)(int16_t)(value & 0xFFFF);
        else                cpu.a[rmw->reg] = value;
        if (rmw->reg == 7) sync_a7_to_sp();
        break;
    default:
        break;
    }
}

void ea_store_value(int mode, int reg, int size, uint32_t value)
{
    uint32_t addr;

    if (mode == 3) {
        /* (An)+ destination: write FIRST, then post-increment only on success.
         * If write fires address error (odd addr), An stays unchanged. */
        addr = cpu.a[reg];
        mem_write_sized(addr, size, value);
        cpu.a[reg] += ea_step(reg, size);
        if (reg == 7) sync_a7_to_sp();
        return;
    }
    if (mode == 4 && size == 4) {
        /* -(An).L destination: two 2-byte steps. If first step lands odd,
         * leave An at An-2 (write fires with An at An-2).
         * Real 68000 does np (prefetch) before -(An) writes; adjust saved_pc by +2. */
        cpu_write_bus_adj = 2;
        cpu.a[reg] -= 2;
        if (!(cpu.a[reg] & 1)) cpu.a[reg] -= 2;
        addr = cpu.a[reg];
        if (reg == 7) sync_a7_to_sp();
        mem_write_sized(addr, size, value);
        return;
    }
    if (mode == 4) {
        /* -(An) destination (byte/word): real 68000 prefetches before write. */
        cpu_write_bus_adj = 2;
    }

    if (ea_resolve_addr(mode, reg, size, &addr)) {
        mem_write_sized(addr, size, value);
        return;
    }

    switch (mode) {
    case 0: /* Dn - upper bits unchanged for byte/word per 68K */
        if (size == 1) cpu.d[reg] = (cpu.d[reg] & 0xFFFFFF00) | (value & 0xFF);
        else if (size == 2) cpu.d[reg] = (cpu.d[reg] & 0xFFFF0000) | (value & 0xFFFF);
        else cpu.d[reg] = value;
        break;
    case 1: /* An */
        if (size == 2) cpu.a[reg] = (uint32_t)(int32_t)(int16_t)(value & 0xFFFF);  /* sign-extend word */
        else cpu.a[reg] = value;
        if (reg == 7)
            sync_a7_to_sp();
        break;
    default:
        break;
    }
}

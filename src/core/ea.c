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

/* Decode indexed addressing mode extension word.
 *
 * Both extension word formats share the same upper byte layout:
 *   [15]    D/A       — 0 = data register, 1 = address register
 *   [14-12] Xn        — index register number (0-7)
 *   [11]    W/L       — 0 = sign-extend Xn as 16-bit word, 1 = use full 32-bit long
 *   [10-9]  SCALE     — shift index left by this many bits (×1/×2/×4/×8)
 *   [8]     EXT TYPE  — 0 = brief extension word, 1 = full extension word (68020+)
 *
 * Brief extension word (bit 8 = 0, all models):
 *   [7-0]   8-bit signed displacement added directly to the address.
 *   Scale bits are always 0 on 68000/68010, so the shift is a no-op there.
 *
 * Full extension word (bit 8 = 1, 68020+ only, guarded by has_full_ea):
 *   [7]     BS — base suppress: if set, ignore the An/PC base and use 0 instead
 *   [6]     IS — index suppress: if set, ignore the Xn index and use 0 instead
 *   [5-4]   BD — base displacement size: 01=none, 10=fetch signed word, 11=fetch signed long
 *   [2]     I/IS — memory indirect flag (not yet implemented; falls through to op_unimplemented)
 *   [1-0]   OD — outer displacement size: 00=none (direct), else memory-indirect
 */
static uint32_t decode_indexed_addr(uint32_t base)
{
    uint16_t ext = fetch16();

    /* Common upper-byte fields shared by both extension word types. */
    int idx_reg     = (ext >> 12) & 7;
    int idx_is_addr = (ext >> 15) & 1;
    int idx_long    = (ext >> 11) & 1;
    int scale       = (ext >> 9) & 3;   /* architecturally defined on 68020+; reserved (ignored) on 68000/010 */

    uint32_t idx_val = idx_is_addr ? cpu.a[idx_reg] : cpu.d[idx_reg];
    if (!idx_long)
        idx_val = (uint32_t)(int32_t)(int16_t)(idx_val & 0xFFFF);  /* sign-extend 16→32 */

    if ((ext & 0x0100) && cpu.features.has_full_ea) {
        /* ---- Full extension word (68020+) ---- */
        int bs    = (ext >> 7) & 1;  /* base suppress */
        int is_   = (ext >> 6) & 1;  /* index suppress */
        int bd_sz = (ext >> 4) & 3;  /* base displacement size field */
        int od_sz =  ext       & 3;  /* outer displacement size field */

        /* Fetch base displacement: none(01), signed word(10), or signed long(11). */
        int32_t bd = 0;
        if (bd_sz == 2) bd = (int32_t)(int16_t)fetch16();
        else if (bd_sz == 3) bd = (int32_t)fetch32();

        /* Memory-indirect modes (OD != 0) require an extra memory read.
         * Not implemented yet — fall through to the illegal-instruction handler. */
        if (od_sz != 0)
            return (uint32_t)op_unimplemented(cpu.ir);

        uint32_t b = bs ? 0 : base;               /* suppress or keep base */
        uint32_t i = is_ ? 0 : (idx_val << scale); /* suppress or scale index */
        return b + (uint32_t)bd + i;
    }

    /* ---- Brief extension word (all models) ---- */
    int32_t disp = (int8_t)(ext & 0xFF);  /* 8-bit signed displacement */
    /* 68000/68010: scale bits (10-9) are reserved; real hardware ignores them.
     * Only apply scale on 68020+ where it is architecturally defined. */
    if (cpu.features.has_full_ea)
        idx_val <<= scale;
    return base + (uint32_t)disp + idx_val;
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
        pending_cycles += 2;
        cpu.a[reg] -= ea_step(reg, size);
        *addr = cpu.a[reg];
        if (reg == 7) {
            if (cpu.sr & 0x2000) cpu.ssp = cpu.a[7];
            else cpu.usp = cpu.a[7];
        }
        return 1;
    case 5: /* d(An) */
        pending_cycles += 4;
        *addr = cpu.a[reg] + (int32_t)(int16_t)fetch16();
        return 1;
    case 6: /* (d8,An,Xn) */
        pending_cycles += 6;
        *addr = decode_indexed_addr(cpu.a[reg]);
        return 1;
    case 7:
        switch (reg) {
        case 0: /* abs.w */
            pending_cycles += 4;
            *addr = (int32_t)(int16_t)fetch16();
            return 1;
        case 1: /* abs.l */
            pending_cycles += 8;
            *addr = fetch32();
            return 1;
        case 2: /* d(PC) */
            pending_cycles += 4;
            *addr = cpu.pc + (int32_t)(int16_t)fetch16();
            return 1;
        case 3: /* (d8,PC,Xn) */
            pending_cycles += 6;
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

    if (ea_resolve_addr(mode, reg, size, &addr)) {
        uint32_t val = mem_read_sized(addr, size);
        pending_cycles += (size <= 2) ? 4 : 8;
        return val;
    }

    switch (mode) {
    case 0: /* Dn */
        return cpu.d[reg];
    case 1: /* An */
        return cpu.a[reg];
    case 7:
        if (reg == 4) { /* #imm */
            if (size == 1) { pending_cycles += 4; return fetch16() & 0xFF; }
            if (size == 2) { pending_cycles += 4; return fetch16() & 0xFFFF; }
            pending_cycles += 8;
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
    if (rmw->is_mem) {
        uint32_t val = mem_read_sized(rmw->addr, size);
        pending_cycles += (size <= 2) ? 4 : 8;
        return val;
    }
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
        /* -(An).L destination: real 68000 decrements in two word-bus steps.
         * A -= 2, then A -= 2 if even (address error fires at odd).
         * In MUSASHI_DIFF_MODE address errors are suppressed so always do -4. */
        pending_cycles += 4;
        cpu_write_bus_adj = 2;
#ifdef MUSASHI_DIFF_MODE
        cpu.a[reg] -= ea_step(reg, size);
#else
        cpu.a[reg] -= 2;
        if (!(cpu.a[reg] & 1)) cpu.a[reg] -= 2;
#endif
        addr = cpu.a[reg];
        if (reg == 7) sync_a7_to_sp();
        mem_write_sized(addr, size, value);
        return;
    }
    if (mode == 4) {
        /* -(An) destination (byte/word): real 68000 prefetches before write. */
        pending_cycles += 2;
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

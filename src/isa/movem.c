/*
 * MOVEM: move multiple registers to/from memory.
 * Store: 0x4880-0x48BF. EA: (An), -(An), d(An), abs.w, abs.l, d(PC). (An)+ invalid.
 * Load:  0x4C80-0x4CBF. EA: (An), (An)+, d(An), abs.w, abs.l, d(PC). -(An) invalid.
 * Register list (16-bit) follows. Order: D0-D7, A0-A7 for control/postinc. A7-A0, D7-D0 for predec.
 */

#include "cpu_internal.h"
#include "ea.h"
#include "memory.h"
#include "timing.h"

/* EA valid for MOVEM store: (An), -(An), d(An), abs.w, abs.l, d(PC). Not (An)+, Dn, An, #imm. */
int movem_store_ea_valid(int mode, int reg)
{
    if (mode == 0 || mode == 1 || mode == 3)
        return 0;
    if (mode == 7 && reg == 4)
        return 0;
    return 1;
}

/* EA valid for MOVEM load: (An), (An)+, d(An), abs.w, abs.l, d(PC). Not -(An), Dn, An, #imm. */
int movem_load_ea_valid(int mode, int reg)
{
    if (mode == 0 || mode == 1 || mode == 4)
        return 0;
    if (mode == 7 && reg == 4)
        return 0;
    return 1;
}

/* MOVEM reg to mem. 0x4880-0x48BF. */
int op_movem_store(uint16_t op)
{
    int ea_mode = ea_mode_from_op(op);
    int ea_reg = ea_reg_from_op(op);
    if (!movem_store_ea_valid(ea_mode, ea_reg))
        return op_unimplemented(op);

    int size = ((op >> 6) & 1) ? 4 : 2;
    uint16_t mask = fetch16();
    pending_cycles += 4;
    uint32_t addr;
    int step = size;

    if (ea_mode == 4) {
        addr = cpu.a[ea_reg];
        for (int i = 0; i < 16; i++) {
            if (mask & (1u << i)) {
                if (size == 4) {
                    addr -= 2;
                    if (!(addr & 1)) addr -= 2;
                } else {
                    addr -= size;
                }
                uint32_t val;
                if (i < 8)
                    val = cpu.a[7 - i];
                else
                    val = cpu.d[15 - i];
                if (size == 2)
                    mem_write16(addr, (uint16_t)(val & 0xFFFF));
                else
                    mem_write32(addr, val);
                pending_cycles += (size == 2) ? 4 : 8;
            }
        }
        cpu.a[ea_reg] = addr;
        if (ea_reg == 7)
            sync_a7_to_sp();
    } else {
        if (!ea_resolve_addr(ea_mode, ea_reg, 4, &addr))
            return op_unimplemented(op);
        for (int i = 0; i < 16; i++) {
            if (mask & (1u << i)) {
                uint32_t val;
                if (i >= 8)
                    val = cpu.a[i - 8];
                else
                    val = cpu.d[i];
                if (size == 2)
                    mem_write16(addr, (uint16_t)(val & 0xFFFF));
                else
                    mem_write32(addr, val);
                pending_cycles += (size == 2) ? 4 : 8;
                addr += step;
            }
        }
        if (ea_mode == 3) {
            cpu.a[ea_reg] = addr;
            if (ea_reg == 7)
                sync_a7_to_sp();
        }
    }
    {
        int n = __builtin_popcount(mask);
        int per_reg = (size == 4) ? 8 : 4;
        int base;
        switch (ea_mode) {
        case 2: case 3: case 4: base = 8; break;
        case 5: base = 12; break;
        case 6: base = 14; break;
        case 7:
            switch (ea_reg) {
            case 0: case 2: base = 12; break;
            case 1: base = 16; break;
            case 3: base = 14; break;
            default: base = 12; break;
            }
            break;
        default: base = 8; break;
        }
        return base + per_reg * n;
    }
}

/* MOVEM mem to reg. 0x4C80-0x4CBF. */
int op_movem_load(uint16_t op)
{
    int ea_mode = ea_mode_from_op(op);
    int ea_reg = ea_reg_from_op(op);
    if (!movem_load_ea_valid(ea_mode, ea_reg))
        return op_unimplemented(op);

    int size = ((op >> 6) & 1) ? 4 : 2;
    uint16_t mask = fetch16();
    pending_cycles += 4;
    uint32_t addr;
    int step = size;

    if (!ea_resolve_addr(ea_mode, ea_reg, 2, &addr))
        return op_unimplemented(op);

    for (int i = 0; i < 16; i++) {
        if (mask & (1u << i)) {
            uint32_t val;
            if (size == 2) {
                val = mem_read16(addr);
                val = (uint32_t)(int32_t)(int16_t)val;
            } else {
                val = mem_read32(addr);
            }
            pending_cycles += (size == 2) ? 4 : 8;
            if (i >= 8)
                cpu.a[i - 8] = val;
            else
                cpu.d[i] = val;
            addr += step;
        }
    }
    if (ea_mode == 3) {
        cpu.a[ea_reg] = addr;
        if (ea_reg == 7)
            sync_a7_to_sp();
    }
    /* If A7 was loaded from memory (bit 15 of mask), sync ssp/usp with cpu.a[7]. */
    if (mask & 0x8000)
        sync_a7_to_sp();
    {
        int n = __builtin_popcount(mask);
        int per_reg = (size == 4) ? 8 : 4;
        int base;
        switch (ea_mode) {
        case 2: case 3: base = 12; break;
        case 5: base = 16; break;
        case 6: base = 18; break;
        case 7:
            switch (ea_reg) {
            case 0: case 2: base = 16; break;
            case 1: base = 20; break;
            case 3: base = 18; break;
            default: base = 16; break;
            }
            break;
        default: base = 12; break;
        }
        return base + per_reg * n;
    }
}

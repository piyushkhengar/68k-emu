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
    if (mode == 7 && (reg == 3 || reg == 4))
        return 0;
    return 1;
}

/* EA valid for MOVEM load: (An), (An)+, d(An), abs.w, abs.l, d(PC). Not -(An), Dn, An, #imm. */
int movem_load_ea_valid(int mode, int reg)
{
    if (mode == 0 || mode == 1 || mode == 4)
        return 0;
    if (mode == 7 && (reg == 3 || reg == 4))
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
    uint32_t addr;
    int step = size;

    if (ea_mode == 4) {
        /* -(An): reverse order A7-A0, D7-D0. Decrement before each store. A7 uses 2 for word. */
        int step_an = (ea_reg == 7 && size == 2) ? 2 : size;
        addr = cpu.a[ea_reg];
        for (int i = 15; i >= 0; i--) {
            if (mask & (1u << i)) {
                addr -= step_an;
                uint32_t val;
                if (i >= 8)
                    val = cpu.a[i - 8];
                else
                    val = cpu.d[i];
                if (size == 2)
                    mem_write16(addr, (uint16_t)(val & 0xFFFF));
                else
                    mem_write32(addr, val);
            }
        }
        cpu.a[ea_reg] = addr;
        if (ea_reg == 7)
            sync_a7_to_sp();
    } else {
        /* Control mode: D0-D7, A0-A7. Increment after each store. */
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
                addr += step;
            }
        }
        if (ea_mode == 3) {
            cpu.a[ea_reg] = addr;
            if (ea_reg == 7)
                sync_a7_to_sp();
        }
    }
    return 12 + 4 * (int)__builtin_popcount(mask);  /* Approximate */
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
    uint32_t addr;
    int step = size;

    if (!ea_resolve_addr(ea_mode, ea_reg, 4, &addr))
        return op_unimplemented(op);

    for (int i = 0; i < 16; i++) {
        if (mask & (1u << i)) {
            uint32_t val;
            if (size == 2) {
                val = mem_read16(addr);
                val = (uint32_t)(int32_t)(int16_t)val;  /* sign-extend */
            } else {
                val = mem_read32(addr);
            }
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
    return 12 + 4 * (int)__builtin_popcount(mask);  /* Approximate */
}

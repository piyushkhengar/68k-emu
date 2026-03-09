#include "cpu_internal.h"
#include "ea.h"
#include "move.h"
#include "timing.h"

/*
 * MOVE encoding: dest EA in bits 11-6 (mode 9-11, reg 6-8), source EA in bits 5-0 (mode 3-5, reg 0-2).
 * Generic MOVE: fetch from source EA, store to dest EA.
 * Exception: MOVE.L #imm, d(An) has dest extension word (displacement) before source (immediate).
 */

static int op_move_generic(uint16_t op, int size)
{
    int src_mode = (op >> 3) & 7;
    int src_reg = op & 7;
    int dst_mode = (op >> 9) & 7;
    int dst_reg = (op >> 6) & 7;

    uint32_t val = ea_fetch_value(src_mode, src_reg, size);
    ea_store_value(dst_mode, dst_reg, size, val);
    set_nz_from_val(val, size);
    return move_cycles(src_mode, src_reg, dst_mode, dst_reg, size);
}

/* MOVE.L #imm, d(An): dest displacement comes before source immediate in extension words. */
static int op_move_l_imm_disp_an(uint16_t op)
{
    int addr_reg = (op >> 6) & 7;
    int32_t disp = (int16_t)fetch16();
    uint32_t addr = cpu.a[addr_reg] + disp;
    uint32_t val = fetch32();
    mem_write32(addr, val);
    set_nz_from_val(val, 4);
    return move_cycles(7, 4, 5, addr_reg, 4);  /* #imm to d(An) */
}

/*
 * Valid EA combinations (whitelist for reference).
 * Source: Dn (0), (An) (2), (An)+ (3), -(An) (4), d(An) (5), #imm (7,4).
 * Dest:   Dn (0), (An) (2), (An)+ (3), -(An) (4), d(An) (5).
 * MOVE.L #imm,d(An) is special-cased: dest ext word (disp) comes before source (imm).
 */

int dispatch_move_b(uint16_t op)
{
    return op_move_generic(op, 1);
}

int dispatch_move_w(uint16_t op)
{
    return op_move_generic(op, 2);
}

int dispatch_move_l(uint16_t op)
{
    if ((op & 0x003F) == 0x3C && (op & 0x0E00) == 0x0A00)
        return op_move_l_imm_disp_an(op);
    return op_move_generic(op, 4);
}

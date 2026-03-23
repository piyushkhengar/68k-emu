#include "cpu_internal.h"
#include "ea.h"
#include "move.h"
#include "timing.h"

/*
 * MOVE encoding: dest EA in bits 11-6 (mode 9-11, reg 6-8), source EA in bits 5-0 (mode 3-5, reg 0-2).
 * Generic MOVE: fetch from source EA, store to dest EA.
 * Extension words are always source-before-destination per 68000 spec.
 */

static int op_move_generic(uint16_t op, int size)
{
    int src_mode = ea_mode_from_op(op);
    int src_reg = ea_reg_from_op(op);
    int dst_mode = ea_mode_from_op_dest(op);
    int dst_reg = ea_reg_from_op_dest(op);

    uint32_t val = ea_fetch_value(src_mode, src_reg, size);
    /* MOVEA (dst_mode==1) does not affect condition codes */
    /* Set CC before the write so address-error on odd write still reflects correct CC */
    if (dst_mode != 1)
        set_nz_from_val(val, size);

    /* For abs.l destination with a memory source, the first destination
     * extension word fetch overlaps with the source read pipeline, so only
     * 4 of the 8 address-fetch cycles are "new" pre-fault cycles.
     * Also, saved_pc is 2 less than the default formula. */
    if (dst_mode == 7 && dst_reg == 1 &&
        src_mode >= 2 && !(src_mode == 7 && src_reg == 4)) {
        pending_cycles -= 4;
        cpu_write_bus_adj = -2;
    }

    ea_store_value(dst_mode, dst_reg, size, val);
    return move_cycles(src_mode, src_reg, dst_mode, dst_reg, size);
}

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
    return op_move_generic(op, 4);
}

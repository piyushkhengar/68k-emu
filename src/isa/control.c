#include "cpu_internal.h"
#include "control.h"
#include "ea.h"
#include "timing.h"

/* NOP: no operation. 0x4E71. */
static int op_nop(uint16_t op)
{
    (void)op;
    return CYCLES_NOP;
}

/* RTS: pop return address from stack, jump to it. 0x4E75. */
static int op_rts(uint16_t op)
{
    (void)op;
    cpu.pc = mem_read32(cpu.a[7]);
    cpu.a[7] += 4;
    return CYCLES_RTS;
}

#define CYCLES_RTE  20

/* TRAP #n: software interrupt. 0x4E40-0x4E4F. Vector 32+n. */
static int op_trap(uint16_t op)
{
    int n = op & 0x0F;
    cpu_take_exception(32 + n, 4);  /* 4 cycles for opcode fetch */
    return 0;  /* unreachable */
}

/* RTE: return from exception. 0x4E73. Supervisor only. */
static int op_rte(uint16_t op)
{
    (void)op;
    /* Privilege violation if not in supervisor mode (SR bit 13) */
    if (!(cpu.sr & 0x2000)) {
        cpu_take_exception(PRIVILEGE_VECTOR, 4);
        return 0;
    }
    uint32_t sp = cpu.a[7];
    cpu.sr = mem_read16(sp);
    cpu.pc = mem_read32(sp + 2);
    cpu.a[7] = sp + 6;
    return CYCLES_RTE;
}

/* Size from op bits 7-6: 00=1, 01=2, 10=4. Used by TST, CLR, NOT. */
static int decode_size_from_op(uint16_t op)
{
    int c = (op >> 6) & 3;
    return (c == 0) ? 1 : (c == 1) ? 2 : 4;
}

/* Decoded fields for NOT. An not allowed. */
typedef struct {
    int ea_mode;
    int ea_reg;
    int size;
    uint32_t mask;
} not_decoded_t;

/* Returns 0 if rejected, 1 if OK to proceed. */
static int decode_not(uint16_t op, not_decoded_t *d)
{
    d->ea_mode = (op >> 3) & 7;
    d->ea_reg = op & 7;
    d->size = decode_size_from_op(op);
    d->mask = (d->size == 1) ? 0xFF : (d->size == 2) ? 0xFFFF : 0xFFFFFFFF;
    if (d->ea_mode == 1) {
        op_unimplemented(op);
        return 0;
    }
    return 1;
}

/* NOT <ea>: one's complement. 0x46xx. An not allowed. */
static int op_not(uint16_t op)
{
    not_decoded_t d;
    if (!decode_not(op, &d))
        return 0;

    uint32_t val = ea_fetch_value(d.ea_mode, d.ea_reg, d.size) & d.mask;
    uint32_t result = (~val) & d.mask;
    ea_store_value(d.ea_mode, d.ea_reg, d.size, result);
    set_nz_from_val(result, d.size);
    return add_sub_cycles(d.ea_mode, d.ea_reg, d.size, 1);
}

/* Decode EA for JMP/JSR. Fills *addr_out, *ea_mode, *ea_reg on success.
 * On invalid EA (Dn, An, #imm), calls op_unimplemented (does not return). */
static void decode_ea_addr_jmp_jsr(uint16_t op, uint32_t *addr_out, int *ea_mode, int *ea_reg)
{
    *ea_mode = (op >> 3) & 7;
    *ea_reg = op & 7;
    if (*ea_mode == 0 || *ea_mode == 1 || (*ea_mode == 7 && *ea_reg == 4))
        op_unimplemented(op);
    if (!ea_address_no_fetch(*ea_mode, *ea_reg, addr_out))
        op_unimplemented(op);
}

/* LEA <ea>, An. 0x41xx. Invalid: Dn, An, (An)+, -(An), #imm. */
static int op_lea(uint16_t op)
{
    int an_reg = (op >> 9) & 7;
    int ea_mode = (op >> 3) & 7;
    int ea_reg = op & 7;

    if (ea_mode == 0 || ea_mode == 1 || ea_mode == 3 || ea_mode == 4)
        return op_unimplemented(op);
    if (ea_mode == 7 && ea_reg == 4)
        return op_unimplemented(op);

    uint32_t addr;
    if (!ea_address_no_fetch(ea_mode, ea_reg, &addr))
        return op_unimplemented(op);

    cpu.a[an_reg] = addr;
    return lea_cycles(ea_mode, ea_reg);
}

/* JMP <ea>. 0x4EC0-0x4EFF. Invalid: Dn, An, #imm. */
static int op_jmp(uint16_t op)
{
    uint32_t addr;
    int ea_mode, ea_reg;
    decode_ea_addr_jmp_jsr(op, &addr, &ea_mode, &ea_reg);
    cpu.pc = addr;
    return jmp_cycles(ea_mode, ea_reg);
}

/* JSR <ea>. 0x4E80-0x4EBF. Invalid: Dn, An, #imm. */
static int op_jsr(uint16_t op)
{
    uint32_t addr;
    int ea_mode, ea_reg;
    decode_ea_addr_jmp_jsr(op, &addr, &ea_mode, &ea_reg);
    cpu.a[7] -= 4;
    mem_write32(cpu.a[7], cpu.pc);
    cpu.pc = addr;
    return jsr_cycles(ea_mode, ea_reg);
}

/* TST <ea>. 0x4Axx. Compare with zero, set N,Z, clear V,C. */
static int op_tst(uint16_t op)
{
    int ea_mode = (op >> 3) & 7;
    int ea_reg = op & 7;
    int size = decode_size_from_op(op);

    uint32_t val = ea_fetch_value(ea_mode, ea_reg, size);
    set_nz_from_val(val, size);
    cpu.sr &= ~(SR_V | SR_C);
    return tst_cycles(ea_mode, ea_reg, size);
}

/* CLR <ea>. 0x42xx. Store 0, set Z, clear N,V,C. An+byte illegal. */
static int op_clr(uint16_t op)
{
    int ea_mode = (op >> 3) & 7;
    int ea_reg = op & 7;
    int size = decode_size_from_op(op);

    if (ea_mode == 1 && size == 1)
        return op_unimplemented(op);

    ea_store_value(ea_mode, ea_reg, size, 0);
    cpu.sr &= ~(SR_N | SR_V | SR_C);
    cpu.sr |= SR_Z;
    return clr_cycles(ea_mode, ea_reg, size);
}

#define CYCLES_EXT_SWAP  4
#define CYCLES_LINK      16
#define CYCLES_UNLK      12

/* EXT.W Dn: sign-extend byte to word. EXT.L Dn: sign-extend word to long. 0x4880-0x48FF. */
static int op_ext(uint16_t op)
{
    int dn = op & 7;
    int opmode = (op >> 6) & 3;
    uint32_t result;

    if (opmode == 2) {
        /* EXT.W: byte -> word */
        int8_t b = (int8_t)(cpu.d[dn] & 0xFF);
        result = (uint32_t)(int32_t)(int16_t)b;
    } else if (opmode == 3) {
        /* EXT.L: word -> long */
        int16_t w = (int16_t)(cpu.d[dn] & 0xFFFF);
        result = (uint32_t)(int32_t)w;
    } else {
        return op_unimplemented(op);
    }
    cpu.d[dn] = result;
    cpu.sr &= ~(SR_N | SR_Z | SR_V | SR_C);
    set_nz_from_val(result, opmode == 2 ? 2 : 4);
    return CYCLES_EXT_SWAP;
}

/* SWAP Dn: exchange upper and lower 16 bits. 0x4840-0x4847. */
static int op_swap(uint16_t op)
{
    int dn = op & 7;
    uint32_t val = cpu.d[dn];
    uint32_t result = ((val >> 16) & 0xFFFF) | ((val & 0xFFFF) << 16);
    cpu.d[dn] = result;
    cpu.sr &= ~(SR_N | SR_Z | SR_V | SR_C);
    set_nz_from_val(result, 4);
    return CYCLES_EXT_SWAP;
}

/* LINK An, #disp: push An, An=SP, SP+=disp. 0x4E50-0x4E57. Word displacement. */
static int op_link(uint16_t op)
{
    int an = op & 7;
    int32_t disp = (int16_t)fetch16();
    cpu.a[7] -= 4;
    mem_write32(cpu.a[7], cpu.a[an]);
    cpu.a[an] = cpu.a[7];
    cpu.a[7] += disp;
    return CYCLES_LINK;
}

/* UNLK An: SP=An, An=pop. 0x4E58-0x4E5F. */
static int op_unlk(uint16_t op)
{
    int an = op & 7;
    cpu.a[7] = cpu.a[an];
    cpu.a[an] = mem_read32(cpu.a[7]);
    cpu.a[7] += 4;
    return CYCLES_UNLK;
}

/* 0x4xxx: LINK, UNLK, JSR, JMP, TRAP, RTE, RTS, NOP, LEA, EXT, SWAP, TST, CLR, NOT. */
int dispatch_4xxx(uint16_t op)
{
    if ((op & 0xFFF8) == 0x4E50) return op_link(op);
    if ((op & 0xFFF8) == 0x4E58) return op_unlk(op);
    if ((op & 0xFFC0) == 0x4E80) return op_jsr(op);
    if ((op & 0xFFC0) == 0x4EC0) return op_jmp(op);
    if ((op & 0xFFF0) == 0x4E40) return op_trap(op);
    if (op == 0x4E73) return op_rte(op);
    if (op == 0x4E75) return op_rts(op);
    if (op == 0x4E71) return op_nop(op);
    if ((op >> 8) >= 0x41 && (op >> 8) <= 0x4F && ((op >> 8) & 1)) return op_lea(op);  /* LEA */
    if ((op & 0xFF80) == 0x4880) return op_ext(op);
    if ((op & 0xFFF8) == 0x4840) return op_swap(op);
    if (op == 0x4AFC) return op_unimplemented(op);  /* ILLEGAL: force vector 4 */
    if ((op & 0xFF00) == 0x4A00) return op_tst(op);
    if ((op & 0xFF00) == 0x4200) return op_clr(op);
    if ((op & 0xFF00) == 0x4600) return op_not(op);
    return op_unimplemented(op);
}

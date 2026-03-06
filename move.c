#include "cpu_internal.h"
#include "move.h"

/* --- MOVE instruction handlers --- */

static void op_move_l_dn_dn(uint16_t op)
{
    uint32_t x = op - 0x2000;
    int dest_reg = (x >> 3) & 7;
    int source_reg = x & 7;
    cpu.d[dest_reg] = cpu.d[source_reg];
    set_nz_from_val(cpu.d[dest_reg], 4);
}

static void op_move_w_dn_dn(uint16_t op)
{
    int dest_reg = (op >> 6) & 7;
    int source_reg = op & 7;
    uint32_t val = cpu.d[source_reg] & 0xFFFF;
    cpu.d[dest_reg] = (cpu.d[dest_reg] & 0xFFFF0000) | val;
    set_nz_from_val(val, 2);
}

static void op_move_b_dn_dn(uint16_t op)
{
    int dest_reg = (op >> 6) & 7;
    int source_reg = op & 7;
    uint32_t val = cpu.d[source_reg] & 0xFF;
    cpu.d[dest_reg] = (cpu.d[dest_reg] & 0xFFFFFF00) | val;
    set_nz_from_val(val, 1);
}

static void op_move_w_an_dn(uint16_t op)
{
    int dest_reg = (op >> 6) & 7;
    int addr_reg = op & 7;
    uint32_t addr = cpu.a[addr_reg];
    uint32_t val = mem_read16(addr) & 0xFFFF;
    cpu.d[dest_reg] = (cpu.d[dest_reg] & 0xFFFF0000) | val;
    set_nz_from_val(val, 2);
}

static void op_move_b_an_dn(uint16_t op)
{
    int dest_reg = (op >> 6) & 7;
    int addr_reg = op & 7;
    uint32_t addr = cpu.a[addr_reg];
    uint32_t val = mem_read8(addr) & 0xFF;
    cpu.d[dest_reg] = (cpu.d[dest_reg] & 0xFFFFFF00) | val;
    set_nz_from_val(val, 1);
}

static void op_move_w_dn_an(uint16_t op)
{
    int source_reg = op & 7;
    int addr_reg = (op >> 6) & 7;
    uint32_t addr = cpu.a[addr_reg];
    uint32_t val = cpu.d[source_reg] & 0xFFFF;
    mem_write16(addr, (uint16_t)val);
    set_nz_from_val(val, 2);
}

static void op_move_b_dn_an(uint16_t op)
{
    int source_reg = op & 7;
    int addr_reg = (op >> 6) & 7;
    uint32_t addr = cpu.a[addr_reg];
    uint32_t val = cpu.d[source_reg] & 0xFF;
    mem_write8(addr, (uint8_t)val);
    set_nz_from_val(val, 1);
}

static void op_move_b_imm_dn(uint16_t op)
{
    int dest_reg = (op >> 6) & 7;
    uint32_t val = fetch16() & 0xFF;
    cpu.d[dest_reg] = (cpu.d[dest_reg] & 0xFFFFFF00) | val;
    set_nz_from_val(val, 1);
}

static void op_move_w_imm_dn(uint16_t op)
{
    int dest_reg = (op >> 6) & 7;
    uint32_t val = fetch16() & 0xFFFF;
    cpu.d[dest_reg] = (cpu.d[dest_reg] & 0xFFFF0000) | val;
    set_nz_from_val(val, 2);
}

static void op_move_l_imm_dn(uint16_t op)
{
    int dest_reg = (op >> 6) & 7;
    uint32_t val = fetch32();
    cpu.d[dest_reg] = val;
    set_nz_from_val(val, 4);
}

static void op_move_b_imm_an(uint16_t op)
{
    int addr_reg = (op >> 6) & 7;
    uint32_t addr = cpu.a[addr_reg];
    uint32_t val = fetch16() & 0xFF;
    mem_write8(addr, (uint8_t)val);
    set_nz_from_val(val, 1);
}

static void op_move_w_imm_an(uint16_t op)
{
    int addr_reg = (op >> 6) & 7;
    uint32_t addr = cpu.a[addr_reg];
    uint32_t val = fetch16() & 0xFFFF;
    mem_write16(addr, (uint16_t)val);
    set_nz_from_val(val, 2);
}

static void op_move_l_imm_an(uint16_t op)
{
    int addr_reg = (op >> 6) & 7;
    uint32_t addr = cpu.a[addr_reg];
    uint32_t val = fetch32();
    mem_write32(addr, val);
    set_nz_from_val(val, 4);
}

static void op_move_l_imm_disp_an(uint16_t op)
{
    int addr_reg = (op >> 6) & 7;
    int32_t disp = (int16_t)fetch16();
    uint32_t addr = cpu.a[addr_reg] + disp;
    uint32_t val = fetch32();
    mem_write32(addr, val);
    set_nz_from_val(val, 4);
}

static void op_move_l_anp_dn(uint16_t op)
{
    int dest_reg = (op >> 6) & 7;
    int addr_reg = op & 7;
    uint32_t addr = cpu.a[addr_reg];
    uint32_t val = mem_read32(addr);
    cpu.d[dest_reg] = val;
    cpu.a[addr_reg] += 4;
    set_nz_from_val(val, 4);
}

static void op_move_l_pdec_an_dn(uint16_t op)
{
    int dest_reg = (op >> 6) & 7;
    int addr_reg = op & 7;
    cpu.a[addr_reg] -= 4;
    uint32_t addr = cpu.a[addr_reg];
    uint32_t val = mem_read32(addr);
    cpu.d[dest_reg] = val;
    set_nz_from_val(val, 4);
}

static void op_move_l_dn_anp(uint16_t op)
{
    int source_reg = op & 7;
    int addr_reg = (op >> 6) & 7;
    uint32_t addr = cpu.a[addr_reg];
    uint32_t val = cpu.d[source_reg];
    mem_write32(addr, val);
    cpu.a[addr_reg] += 4;
    set_nz_from_val(val, 4);
}

static void op_move_l_dn_pdec_an(uint16_t op)
{
    int source_reg = op & 7;
    int addr_reg = (op >> 6) & 7;
    uint32_t val = cpu.d[source_reg];
    cpu.a[addr_reg] -= 4;
    uint32_t addr = cpu.a[addr_reg];
    mem_write32(addr, val);
    set_nz_from_val(val, 4);
}

static void op_move_w_anp_dn(uint16_t op)
{
    int dest_reg = (op >> 6) & 7;
    int addr_reg = op & 7;
    uint32_t addr = cpu.a[addr_reg];
    uint32_t val = mem_read16(addr) & 0xFFFF;
    cpu.d[dest_reg] = (cpu.d[dest_reg] & 0xFFFF0000) | val;
    cpu.a[addr_reg] += 2;
    set_nz_from_val(val, 2);
}

static void op_move_w_pdec_an_dn(uint16_t op)
{
    int dest_reg = (op >> 6) & 7;
    int addr_reg = op & 7;
    cpu.a[addr_reg] -= 2;
    uint32_t addr = cpu.a[addr_reg];
    uint32_t val = mem_read16(addr) & 0xFFFF;
    cpu.d[dest_reg] = (cpu.d[dest_reg] & 0xFFFF0000) | val;
    set_nz_from_val(val, 2);
}

static void op_move_w_disp_an_dn(uint16_t op)
{
    int dest_reg = (op >> 6) & 7;
    int addr_reg = op & 7;
    int32_t disp = (int16_t)fetch16();
    uint32_t addr = cpu.a[addr_reg] + disp;
    uint32_t val = mem_read16(addr) & 0xFFFF;
    cpu.d[dest_reg] = (cpu.d[dest_reg] & 0xFFFF0000) | val;
    set_nz_from_val(val, 2);
}

static void op_move_w_dn_anp(uint16_t op)
{
    int source_reg = op & 7;
    int addr_reg = (op >> 6) & 7;
    uint32_t addr = cpu.a[addr_reg];
    uint32_t val = cpu.d[source_reg] & 0xFFFF;
    mem_write16(addr, (uint16_t)val);
    cpu.a[addr_reg] += 2;
    set_nz_from_val(val, 2);
}

static void op_move_w_dn_pdec_an(uint16_t op)
{
    int source_reg = op & 7;
    int addr_reg = (op >> 6) & 7;
    uint32_t val = cpu.d[source_reg] & 0xFFFF;
    cpu.a[addr_reg] -= 2;
    uint32_t addr = cpu.a[addr_reg];
    mem_write16(addr, (uint16_t)val);
    set_nz_from_val(val, 2);
}

static void op_move_w_dn_disp_an(uint16_t op)
{
    int source_reg = op & 7;
    int addr_reg = (op >> 6) & 7;
    int32_t disp = (int16_t)fetch16();
    uint32_t addr = cpu.a[addr_reg] + disp;
    uint32_t val = cpu.d[source_reg] & 0xFFFF;
    mem_write16(addr, (uint16_t)val);
    set_nz_from_val(val, 2);
}

static void op_move_b_anp_dn(uint16_t op)
{
    int dest_reg = (op >> 6) & 7;
    int addr_reg = op & 7;
    uint32_t addr = cpu.a[addr_reg];
    uint32_t val = mem_read8(addr) & 0xFF;
    cpu.d[dest_reg] = (cpu.d[dest_reg] & 0xFFFFFF00) | val;
    cpu.a[addr_reg] += (addr_reg == 7) ? 2 : 1;
    set_nz_from_val(val, 1);
}

static void op_move_b_pdec_an_dn(uint16_t op)
{
    int dest_reg = (op >> 6) & 7;
    int addr_reg = op & 7;
    int dec = (addr_reg == 7) ? 2 : 1;
    cpu.a[addr_reg] -= dec;
    uint32_t addr = cpu.a[addr_reg];
    uint32_t val = mem_read8(addr) & 0xFF;
    cpu.d[dest_reg] = (cpu.d[dest_reg] & 0xFFFFFF00) | val;
    set_nz_from_val(val, 1);
}

static void op_move_b_disp_an_dn(uint16_t op)
{
    int dest_reg = (op >> 6) & 7;
    int addr_reg = op & 7;
    int32_t disp = (int16_t)fetch16();
    uint32_t addr = cpu.a[addr_reg] + disp;
    uint32_t val = mem_read8(addr) & 0xFF;
    cpu.d[dest_reg] = (cpu.d[dest_reg] & 0xFFFFFF00) | val;
    set_nz_from_val(val, 1);
}

static void op_move_b_dn_anp(uint16_t op)
{
    int source_reg = op & 7;
    int addr_reg = (op >> 6) & 7;
    uint32_t addr = cpu.a[addr_reg];
    uint32_t val = cpu.d[source_reg] & 0xFF;
    mem_write8(addr, (uint8_t)val);
    cpu.a[addr_reg] += (addr_reg == 7) ? 2 : 1;
    set_nz_from_val(val, 1);
}

static void op_move_b_dn_pdec_an(uint16_t op)
{
    int source_reg = op & 7;
    int addr_reg = (op >> 6) & 7;
    uint32_t val = cpu.d[source_reg] & 0xFF;
    int dec = (addr_reg == 7) ? 2 : 1;
    cpu.a[addr_reg] -= dec;
    uint32_t addr = cpu.a[addr_reg];
    mem_write8(addr, (uint8_t)val);
    set_nz_from_val(val, 1);
}

static void op_move_b_dn_disp_an(uint16_t op)
{
    int source_reg = op & 7;
    int addr_reg = (op >> 6) & 7;
    int32_t disp = (int16_t)fetch16();
    uint32_t addr = cpu.a[addr_reg] + disp;
    uint32_t val = cpu.d[source_reg] & 0xFF;
    mem_write8(addr, (uint8_t)val);
    set_nz_from_val(val, 1);
}

static void op_move_l_disp_an_dn(uint16_t op)
{
    int dest_reg = (op >> 6) & 7;
    int addr_reg = op & 7;
    int32_t disp = (int16_t)fetch16();
    uint32_t addr = cpu.a[addr_reg] + disp;
    uint32_t val = mem_read32(addr);
    cpu.d[dest_reg] = val;
    set_nz_from_val(val, 4);
}

static void op_move_l_dn_disp_an(uint16_t op)
{
    int source_reg = op & 7;
    int addr_reg = (op >> 6) & 7;
    int32_t disp = (int16_t)fetch16();
    uint32_t addr = cpu.a[addr_reg] + disp;
    uint32_t val = cpu.d[source_reg];
    mem_write32(addr, val);
    set_nz_from_val(val, 4);
}

static void op_move_l_an_dn(uint16_t op)
{
    int dest_reg = (op >> 6) & 7;
    int addr_reg = op & 7;
    uint32_t addr = cpu.a[addr_reg];
    uint32_t val = mem_read32(addr);
    cpu.d[dest_reg] = val;
    set_nz_from_val(val, 4);
}

static void op_move_l_dn_an(uint16_t op)
{
    int source_reg = op & 7;
    int addr_reg = (op >> 6) & 7;
    uint32_t addr = cpu.a[addr_reg];
    uint32_t val = cpu.d[source_reg];
    mem_write32(addr, val);
    set_nz_from_val(val, 4);
}

/* Family dispatchers */

void dispatch_move_b(uint16_t op)
{
    if ((op & 0x003F) == 0x3C) {
        if ((op & 0x0E00) == 0) { op_move_b_imm_dn(op); return; }
        if ((op & 0x0E00) == 0x0400) { op_move_b_imm_an(op); return; }
    }
    if ((op & 0x0E00) == 0 && (op & 0x0038) == 0x18) { op_move_b_anp_dn(op); return; }
    if ((op & 0x0E00) == 0 && (op & 0x0038) == 0x20) { op_move_b_pdec_an_dn(op); return; }
    if ((op & 0x0E00) == 0 && (op & 0x0038) == 0x28) { op_move_b_disp_an_dn(op); return; }
    if ((op & 0x0E00) == 0x0400 && (op & 0x0038) == 0x18) { op_move_b_dn_anp(op); return; }
    if ((op & 0x0E00) == 0x0400 && (op & 0x0038) == 0x20) { op_move_b_dn_pdec_an(op); return; }
    if ((op & 0x0E00) == 0x0400 && (op & 0x0038) == 0x28) { op_move_b_dn_disp_an(op); return; }
    if ((op & 0x0E38) == 0) { op_move_b_dn_dn(op); return; }
    if ((op & 0x0E00) == 0 && (op & 0x0038) == 0x10) { op_move_b_an_dn(op); return; }
    if ((op & 0x0E00) == 0x0400 && (op & 0x0038) == 0) { op_move_b_dn_an(op); return; }
    op_unimplemented(op);
}

void dispatch_move_l(uint16_t op)
{
    if ((op & 0x003F) == 0x3C) {
        if ((op & 0x0E00) == 0) { op_move_l_imm_dn(op); return; }
        if ((op & 0x0E00) == 0x0400) { op_move_l_imm_an(op); return; }
        if ((op & 0x0E00) == 0x0A00) { op_move_l_imm_disp_an(op); return; }
    }
    if ((op & 0x0E00) == 0 && (op & 0x0038) == 0x18) { op_move_l_anp_dn(op); return; }
    if ((op & 0x0E00) == 0 && (op & 0x0038) == 0x20) { op_move_l_pdec_an_dn(op); return; }
    if ((op & 0x0E00) == 0 && (op & 0x0038) == 0x28) { op_move_l_disp_an_dn(op); return; }
    if ((op & 0x0E00) == 0x0400 && (op & 0x0038) == 0x18) { op_move_l_dn_anp(op); return; }
    if ((op & 0x0E00) == 0x0400 && (op & 0x0038) == 0x20) { op_move_l_dn_pdec_an(op); return; }
    if ((op & 0x0E00) == 0x0A00 && (op & 0x0038) == 0) { op_move_l_dn_disp_an(op); return; }
    if ((op & 0x0E38) == 0) { op_move_l_dn_dn(op); return; }
    if ((op & 0x0E00) == 0 && (op & 0x0038) == 0x10) { op_move_l_an_dn(op); return; }
    if ((op & 0x0E00) == 0x0400 && (op & 0x0038) == 0) { op_move_l_dn_an(op); return; }
    op_unimplemented(op);
}

void dispatch_move_w(uint16_t op)
{
    if ((op & 0x003F) == 0x3C) {
        if ((op & 0x0E00) == 0) { op_move_w_imm_dn(op); return; }
        if ((op & 0x0E00) == 0x0400) { op_move_w_imm_an(op); return; }
    }
    if ((op & 0x0E00) == 0 && (op & 0x0038) == 0x18) { op_move_w_anp_dn(op); return; }
    if ((op & 0x0E00) == 0 && (op & 0x0038) == 0x20) { op_move_w_pdec_an_dn(op); return; }
    if ((op & 0x0E00) == 0 && (op & 0x0038) == 0x28) { op_move_w_disp_an_dn(op); return; }
    if ((op & 0x0E00) == 0x0400 && (op & 0x0038) == 0x18) { op_move_w_dn_anp(op); return; }
    if ((op & 0x0E00) == 0x0400 && (op & 0x0038) == 0x20) { op_move_w_dn_pdec_an(op); return; }
    if ((op & 0x0E00) == 0x0400 && (op & 0x0038) == 0x28) { op_move_w_dn_disp_an(op); return; }
    if ((op & 0x0E38) == 0) { op_move_w_dn_dn(op); return; }
    if ((op & 0x0E00) == 0 && (op & 0x0038) == 0x10) { op_move_w_an_dn(op); return; }
    if ((op & 0x0E00) == 0x0400 && (op & 0x0038) == 0) { op_move_w_dn_an(op); return; }
    op_unimplemented(op);
}

#include "cpu_internal.h"
#include "move.h"

/* --- MOVE instruction handlers --- */

/* MOVE.L Dn, Dn: copy 32-bit value between data registers. Dest in bits 8-6, source in 2-0. */
static void op_move_l_dn_dn(uint16_t op)
{
    uint32_t x = op - 0x2000;
    int dest_reg = (x >> 3) & 7;
    int source_reg = x & 7;
    cpu.d[dest_reg] = cpu.d[source_reg];
    set_nz_from_val(cpu.d[dest_reg], 4);
}

/* MOVE.W Dn, Dn: copy low 16 bits; upper 16 bits of dest unchanged. Dest in bits 11-6, source in 5-0. */
static void op_move_w_dn_dn(uint16_t op)
{
    int dest_reg = (op >> 6) & 7;
    int source_reg = op & 7;
    uint32_t val = cpu.d[source_reg] & 0xFFFF;
    cpu.d[dest_reg] = (cpu.d[dest_reg] & 0xFFFF0000) | val;
    set_nz_from_val(val, 2);
}

/* MOVE.B Dn, Dn: copy low 8 bits; upper 24 bits of dest unchanged. Dest in bits 11-6, source in 5-0. */
static void op_move_b_dn_dn(uint16_t op)
{
    int dest_reg = (op >> 6) & 7;
    int source_reg = op & 7;
    uint32_t val = cpu.d[source_reg] & 0xFF;
    cpu.d[dest_reg] = (cpu.d[dest_reg] & 0xFFFFFF00) | val;
    set_nz_from_val(val, 1);
}

/* MOVE.W (An), Dn: load word from memory at address in An. */
static void op_move_w_an_dn(uint16_t op)
{
    int dest_reg = (op >> 6) & 7;
    int addr_reg = op & 7;
    uint32_t addr = cpu.a[addr_reg];
    uint32_t val = mem_read16(addr) & 0xFFFF;
    cpu.d[dest_reg] = (cpu.d[dest_reg] & 0xFFFF0000) | val;
    set_nz_from_val(val, 2);
}

/* MOVE.B (An), Dn: load byte from memory at address in An. */
static void op_move_b_an_dn(uint16_t op)
{
    int dest_reg = (op >> 6) & 7;
    int addr_reg = op & 7;
    uint32_t addr = cpu.a[addr_reg];
    uint32_t val = mem_read8(addr) & 0xFF;
    cpu.d[dest_reg] = (cpu.d[dest_reg] & 0xFFFFFF00) | val;
    set_nz_from_val(val, 1);
}

/* MOVE.W Dn, (An): store word to memory at address in An. */
static void op_move_w_dn_an(uint16_t op)
{
    int source_reg = op & 7;
    int addr_reg = (op >> 6) & 7;
    uint32_t addr = cpu.a[addr_reg];
    uint32_t val = cpu.d[source_reg] & 0xFFFF;
    mem_write16(addr, (uint16_t)val);
    set_nz_from_val(val, 2);
}

/* MOVE.B Dn, (An): store byte to memory at address in An. */
static void op_move_b_dn_an(uint16_t op)
{
    int source_reg = op & 7;
    int addr_reg = (op >> 6) & 7;
    uint32_t addr = cpu.a[addr_reg];
    uint32_t val = cpu.d[source_reg] & 0xFF;
    mem_write8(addr, (uint8_t)val);
    set_nz_from_val(val, 1);
}

/* MOVE.B #imm, Dn: fetch word, use low byte. Source EA 0x3C (mode 7 reg 4). */
static void op_move_b_imm_dn(uint16_t op)
{
    int dest_reg = (op >> 6) & 7;
    uint32_t val = fetch16() & 0xFF;
    cpu.d[dest_reg] = (cpu.d[dest_reg] & 0xFFFFFF00) | val;
    set_nz_from_val(val, 1);
}

/* MOVE.W #imm, Dn: fetch one extension word as immediate. */
static void op_move_w_imm_dn(uint16_t op)
{
    int dest_reg = (op >> 6) & 7;
    uint32_t val = fetch16() & 0xFFFF;
    cpu.d[dest_reg] = (cpu.d[dest_reg] & 0xFFFF0000) | val;
    set_nz_from_val(val, 2);
}

/* MOVE.L #imm, Dn: fetch 32-bit immediate. */
static void op_move_l_imm_dn(uint16_t op)
{
    int dest_reg = (op >> 6) & 7;
    uint32_t val = fetch32();
    cpu.d[dest_reg] = val;
    set_nz_from_val(val, 4);
}

/* MOVE.B #imm, (An): fetch word, store low byte to memory at An. */
static void op_move_b_imm_an(uint16_t op)
{
    int addr_reg = (op >> 6) & 7;
    uint32_t addr = cpu.a[addr_reg];
    uint32_t val = fetch16() & 0xFF;
    mem_write8(addr, (uint8_t)val);
    set_nz_from_val(val, 1);
}

/* MOVE.W #imm, (An): fetch word, store to memory at An. */
static void op_move_w_imm_an(uint16_t op)
{
    int addr_reg = (op >> 6) & 7;
    uint32_t addr = cpu.a[addr_reg];
    uint32_t val = fetch16() & 0xFFFF;
    mem_write16(addr, (uint16_t)val);
    set_nz_from_val(val, 2);
}

/* MOVE.L #imm, (An): fetch 32-bit immediate, store to memory at An. */
static void op_move_l_imm_an(uint16_t op)
{
    int addr_reg = (op >> 6) & 7;
    uint32_t addr = cpu.a[addr_reg];
    uint32_t val = fetch32();
    mem_write32(addr, val);
    set_nz_from_val(val, 4);
}

/* MOVE.L #imm, d(An): fetch 16-bit displacement, then 32-bit immediate; store to An+disp. */
static void op_move_l_imm_disp_an(uint16_t op)
{
    int addr_reg = (op >> 6) & 7;
    int32_t disp = (int16_t)fetch16();
    uint32_t addr = cpu.a[addr_reg] + disp;
    uint32_t val = fetch32();
    mem_write32(addr, val);
    set_nz_from_val(val, 4);
}

/* MOVE.L (An)+, Dn: load from An, then increment An by 4. Post-increment (mode 3). */
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

/* MOVE.L -(An), Dn: decrement An by 4, then load. Pre-decrement (mode 4). */
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

/* MOVE.L Dn, (An)+: store to An, then increment An by 4. Post-increment. */
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

/* MOVE.L Dn, -(An): decrement An by 4, then store. Pre-decrement. */
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

/* MOVE.W (An)+, Dn: load word from An, then increment An by 2. */
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

/* MOVE.W -(An), Dn: decrement An by 2, then load word. */
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

/* MOVE.W d(An), Dn: fetch 16-bit displacement, load word from An+disp. Mode 5. */
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

/* MOVE.W Dn, (An)+: store word to An, then increment An by 2. */
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

/* MOVE.W Dn, -(An): decrement An by 2, then store word. */
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

/* MOVE.W Dn, d(An): fetch 16-bit displacement, store word to An+disp. */
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

/* MOVE.B (An)+, Dn: load byte from An, increment An by 1 (A7 by 2 for word alignment). */
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

/* MOVE.B -(An), Dn: decrement An by 1 (A7 by 2), then load byte. */
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

/* MOVE.B d(An), Dn: fetch 16-bit displacement, load byte from An+disp. */
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

/* MOVE.B Dn, (An)+: store byte to An, increment An by 1 (A7 by 2). */
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

/* MOVE.B Dn, -(An): decrement An by 1 (A7 by 2), then store byte. */
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

/* MOVE.B Dn, d(An): fetch 16-bit displacement, store byte to An+disp. */
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

/* MOVE.L d(An), Dn: fetch 16-bit displacement, load long from An+disp. Mode 5. */
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

/* MOVE.L Dn, d(An): fetch 16-bit displacement, store long to An+disp. */
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

/* MOVE.L (An), Dn: load long from memory at address in An. Dest Dn (mode 0), source (An) (mode 2). */
static void op_move_l_an_dn(uint16_t op)
{
    int dest_reg = (op >> 6) & 7;
    int addr_reg = op & 7;
    uint32_t addr = cpu.a[addr_reg];
    uint32_t val = mem_read32(addr);
    cpu.d[dest_reg] = val;
    set_nz_from_val(val, 4);
}

/* MOVE.L Dn, (An): store long to memory at address in An. Source Dn (mode 0), dest (An) (mode 2). */
static void op_move_l_dn_an(uint16_t op)
{
    int source_reg = op & 7;
    int addr_reg = (op >> 6) & 7;
    uint32_t addr = cpu.a[addr_reg];
    uint32_t val = cpu.d[source_reg];
    mem_write32(addr, val);
    set_nz_from_val(val, 4);
}

/* MOVE.B 0x1xxx: dispatch by dest/source EA modes. */

void dispatch_move_b(uint16_t op)
{
    if ((op & 0x003F) == 0x3C) {
        if ((op & 0x0E00) == 0) { op_move_b_imm_dn(op); return; }
        if ((op & 0x0E00) == 0x0400) { op_move_b_imm_an(op); return; }
    }
    if ((op & 0x0E00) == 0 && (op & 0x0038) == 0x18) { op_move_b_anp_dn(op); return; }
    if ((op & 0x0E00) == 0 && (op & 0x0038) == 0x20) { op_move_b_pdec_an_dn(op); return; }
    if ((op & 0x0E00) == 0 && (op & 0x0038) == 0x28) { op_move_b_disp_an_dn(op); return; }
    if ((op & 0x0E00) == 0x0600 && (op & 0x0038) == 0) { op_move_b_dn_anp(op); return; }
    if ((op & 0x0E00) == 0x0800 && (op & 0x0038) == 0) { op_move_b_dn_pdec_an(op); return; }
    if ((op & 0x0E00) == 0x0A00 && (op & 0x0038) == 0) { op_move_b_dn_disp_an(op); return; }
    if ((op & 0x0E38) == 0) { op_move_b_dn_dn(op); return; }
    if ((op & 0x0E00) == 0 && (op & 0x0038) == 0x10) { op_move_b_an_dn(op); return; }
    if ((op & 0x0E00) == 0x0400 && (op & 0x0038) == 0) { op_move_b_dn_an(op); return; }
    op_unimplemented(op);
}

/* MOVE.L 0x2xxx: dispatch by dest/source EA modes. */
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
    if ((op & 0x0E00) == 0x0600 && (op & 0x0038) == 0) { op_move_l_dn_anp(op); return; }
    if ((op & 0x0E00) == 0x0800 && (op & 0x0038) == 0) { op_move_l_dn_pdec_an(op); return; }
    if ((op & 0x0E00) == 0x0A00 && (op & 0x0038) == 0) { op_move_l_dn_disp_an(op); return; }
    if ((op & 0x0E38) == 0) { op_move_l_dn_dn(op); return; }
    if ((op & 0x0E00) == 0 && (op & 0x0038) == 0x10) { op_move_l_an_dn(op); return; }
    if ((op & 0x0E00) == 0x0400 && (op & 0x0038) == 0) { op_move_l_dn_an(op); return; }
    op_unimplemented(op);
}

/* MOVE.W 0x3xxx: dispatch by dest/source EA modes. */
void dispatch_move_w(uint16_t op)
{
    if ((op & 0x003F) == 0x3C) {
        if ((op & 0x0E00) == 0) { op_move_w_imm_dn(op); return; }
        if ((op & 0x0E00) == 0x0400) { op_move_w_imm_an(op); return; }
    }
    if ((op & 0x0E00) == 0 && (op & 0x0038) == 0x18) { op_move_w_anp_dn(op); return; }
    if ((op & 0x0E00) == 0 && (op & 0x0038) == 0x20) { op_move_w_pdec_an_dn(op); return; }
    if ((op & 0x0E00) == 0 && (op & 0x0038) == 0x28) { op_move_w_disp_an_dn(op); return; }
    if ((op & 0x0E00) == 0x0600 && (op & 0x0038) == 0) { op_move_w_dn_anp(op); return; }
    if ((op & 0x0E00) == 0x0800 && (op & 0x0038) == 0) { op_move_w_dn_pdec_an(op); return; }
    if ((op & 0x0E00) == 0x0A00 && (op & 0x0038) == 0) { op_move_w_dn_disp_an(op); return; }
    if ((op & 0x0E38) == 0) { op_move_w_dn_dn(op); return; }
    if ((op & 0x0E00) == 0 && (op & 0x0038) == 0x10) { op_move_w_an_dn(op); return; }
    if ((op & 0x0E00) == 0x0400 && (op & 0x0038) == 0) { op_move_w_dn_an(op); return; }
    op_unimplemented(op);
}

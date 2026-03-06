#include "cpu.h"
#include "memory.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

CPU cpu;

void cpu_init(void)
{
    memset(&cpu, 0, sizeof(cpu));
}

void cpu_reset(void)
{
    /* 68K fetches reset vector at 0x000000: PC, then SP */
    cpu.pc = mem_read32(0);
    cpu.a[7] = mem_read32(4);  /* A7 = stack pointer */
    cpu.sr = 0x2700;           /* Supervisor mode, interrupts disabled */
    cpu.halted = 0;
    cpu.cycles = 0;
}

/*
 * Fetch a word at PC and advance PC.
 * 68K is big-endian; mem_read16 already returns big-endian.
 */
static inline uint16_t fetch16(void)
{
    uint16_t w = mem_read16(cpu.pc);
    cpu.pc += 2;
    return w;
}

static inline uint32_t fetch32(void)
{
    uint32_t w = mem_read32(cpu.pc);
    cpu.pc += 4;
    return w;
}

/* Helper: set N,Z and clear V,C from value (size in bytes: 1,2,4) */
static void set_nz_from_val(uint32_t val, int size)
{
    cpu.sr &= ~(SR_N | SR_Z | SR_V | SR_C);
    if (val == 0)
        cpu.sr |= SR_Z;
    if (size == 1 && (val & 0x80))
        cpu.sr |= SR_N;
    else if (size == 2 && (val & 0x8000))
        cpu.sr |= SR_N;
    else if (size == 4 && (val & 0x80000000))
        cpu.sr |= SR_N;
}

/* Helper: set N,Z,V,C from ADD result (dest + source = result) */
static void set_nzvc_add(uint32_t result, uint32_t dest_val, uint32_t source_val)
{
    int32_t a = (int32_t)dest_val, b = (int32_t)source_val, r = (int32_t)result;

    cpu.sr &= ~(SR_N | SR_Z | SR_V | SR_C);
    if (result == 0)
        cpu.sr |= SR_Z;
    if (result & 0x80000000)
        cpu.sr |= SR_N;
    if (result < dest_val)  /* Carry out (unsigned overflow) */
        cpu.sr |= SR_C;
    if ((a > 0 && b > 0 && r < 0) || (a < 0 && b < 0 && r > 0))  /* Signed overflow */
        cpu.sr |= SR_V;
}

/* Helper: set N,Z,V,C from SUB/CMP result (dest - source = result) */
static void set_nzvc_sub(uint32_t result, uint32_t dest_val, uint32_t source_val)
{
    int32_t a = (int32_t)dest_val, b = (int32_t)source_val, r = (int32_t)result;

    cpu.sr &= ~(SR_N | SR_Z | SR_V | SR_C);
    if (result == 0)
        cpu.sr |= SR_Z;
    if (result & 0x80000000)
        cpu.sr |= SR_N;
    if (dest_val < source_val)  /* Borrow (unsigned underflow) */
        cpu.sr |= SR_C;
    if ((a >= 0 && b < 0 && r < 0) || (a < 0 && b >= 0 && r > 0))  /* Signed overflow */
        cpu.sr |= SR_V;
}

/* --- Instruction handlers --- */

static void op_nop(uint16_t op)
{
    (void)op;
    /* NOP: 0x4E71 - do nothing */
}

static void op_rts(uint16_t op)
{
    (void)op;
    /* RTS: 0x4E75 - pop return address from stack, jump to it */
    cpu.pc = mem_read32(cpu.a[7]);
    cpu.a[7] += 4;
}

static void op_move_l_dn_dn(uint16_t op)
{
    /* MOVE.L Ds, Dd: 0x2000 + (d<<3) + s
     * dest_reg = (op - 0x2000) >> 3
     * source_reg = (op - 0x2000) & 7
     */
    uint32_t x = op - 0x2000;
    int dest_reg = (x >> 3) & 7;
    int source_reg = x & 7;
    cpu.d[dest_reg] = cpu.d[source_reg];
    set_nz_from_val(cpu.d[dest_reg], 4);
}

/* MOVE.W Ds, Dd: 0x3xxx, standard EA format - dest in bits 11-6, source in 5-0. Upper 16 bits of Dd unchanged. */
static void op_move_w_dn_dn(uint16_t op)
{
    int dest_reg = (op >> 6) & 7;
    int source_reg = op & 7;
    uint32_t val = cpu.d[source_reg] & 0xFFFF;
    cpu.d[dest_reg] = (cpu.d[dest_reg] & 0xFFFF0000) | val;
    set_nz_from_val(val, 2);
}

/* MOVE.B Ds, Dd: 0x1xxx, standard EA format. Upper 24 bits of Dd unchanged. */
static void op_move_b_dn_dn(uint16_t op)
{
    int dest_reg = (op >> 6) & 7;
    int source_reg = op & 7;
    uint32_t val = cpu.d[source_reg] & 0xFF;
    cpu.d[dest_reg] = (cpu.d[dest_reg] & 0xFFFFFF00) | val;
    set_nz_from_val(val, 1);
}

/* MOVE.W (An), Dn */
static void op_move_w_an_dn(uint16_t op)
{
    int dest_reg = (op >> 6) & 7;
    int addr_reg = op & 7;
    uint32_t addr = cpu.a[addr_reg];
    uint32_t val = mem_read16(addr) & 0xFFFF;

    cpu.d[dest_reg] = (cpu.d[dest_reg] & 0xFFFF0000) | val;
    set_nz_from_val(val, 2);
}

/* MOVE.B (An), Dn */
static void op_move_b_an_dn(uint16_t op)
{
    int dest_reg = (op >> 6) & 7;
    int addr_reg = op & 7;
    uint32_t addr = cpu.a[addr_reg];
    uint32_t val = mem_read8(addr) & 0xFF;

    cpu.d[dest_reg] = (cpu.d[dest_reg] & 0xFFFFFF00) | val;
    set_nz_from_val(val, 1);
}

/* MOVE.W Dn, (An) */
static void op_move_w_dn_an(uint16_t op)
{
    int source_reg = op & 7;
    int addr_reg = (op >> 6) & 7;
    uint32_t addr = cpu.a[addr_reg];
    uint32_t val = cpu.d[source_reg] & 0xFFFF;

    mem_write16(addr, (uint16_t)val);
    set_nz_from_val(val, 2);
}

/* MOVE.B Dn, (An) */
static void op_move_b_dn_an(uint16_t op)
{
    int source_reg = op & 7;
    int addr_reg = (op >> 6) & 7;
    uint32_t addr = cpu.a[addr_reg];
    uint32_t val = cpu.d[source_reg] & 0xFF;

    mem_write8(addr, (uint8_t)val);
    set_nz_from_val(val, 1);
}

/* MOVE.B #imm, Dn: source EA 0x3C (mode 7 reg 4). Fetch 1 word, use low byte. */
static void op_move_b_imm_dn(uint16_t op)
{
    int dest_reg = (op >> 6) & 7;
    uint32_t val = fetch16() & 0xFF;

    cpu.d[dest_reg] = (cpu.d[dest_reg] & 0xFFFFFF00) | val;
    set_nz_from_val(val, 1);
}

/* MOVE.W #imm, Dn: fetch 1 word. */
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

/* MOVE.B #imm, (An): dest (An) in bits 11-6. */
static void op_move_b_imm_an(uint16_t op)
{
    int addr_reg = (op >> 6) & 7;
    uint32_t addr = cpu.a[addr_reg];
    uint32_t val = fetch16() & 0xFF;

    mem_write8(addr, (uint8_t)val);
    set_nz_from_val(val, 1);
}

/* MOVE.W #imm, (An) */
static void op_move_w_imm_an(uint16_t op)
{
    int addr_reg = (op >> 6) & 7;
    uint32_t addr = cpu.a[addr_reg];
    uint32_t val = fetch16() & 0xFFFF;

    mem_write16(addr, (uint16_t)val);
    set_nz_from_val(val, 2);
}

/* MOVE.L #imm, (An) */
static void op_move_l_imm_an(uint16_t op)
{
    int addr_reg = (op >> 6) & 7;
    uint32_t addr = cpu.a[addr_reg];
    uint32_t val = fetch32();

    mem_write32(addr, val);
    set_nz_from_val(val, 4);
}

/* MOVE.L #imm, d(An): dest d(An) needs displacement fetch before immediate */
static void op_move_l_imm_disp_an(uint16_t op)
{
    int addr_reg = (op >> 6) & 7;
    int32_t disp = (int16_t)fetch16();
    uint32_t addr = cpu.a[addr_reg] + disp;
    uint32_t val = fetch32();

    mem_write32(addr, val);
    set_nz_from_val(val, 4);
}

/* MOVE.L (An)+, Dn: mode 3. Load then increment An. */
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

/* MOVE.L -(An), Dn: mode 4. Decrement An then load. */
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

/* MOVE.L Dn, (An)+: store then increment. */
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

/* MOVE.L Dn, -(An): decrement then store. */
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

/* MOVE.W (An)+, Dn */
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

/* MOVE.W -(An), Dn */
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

/* MOVE.W d(An), Dn */
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

/* MOVE.W Dn, (An)+ */
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

/* MOVE.W Dn, -(An) */
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

/* MOVE.W Dn, d(An) */
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

/* MOVE.B (An)+, Dn - A7 increments by 2 bytes for word alignment */
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

/* MOVE.B -(An), Dn - A7 decrements by 2 bytes for word alignment */
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

/* MOVE.B d(An), Dn */
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

/* MOVE.B Dn, (An)+ - A7 increments by 2 bytes for word alignment */
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

/* MOVE.B Dn, -(An) - A7 decrements by 2 bytes for word alignment */
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

/* MOVE.B Dn, d(An) */
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

/* MOVE.L d(An), Dn: mode 5. Fetch 16-bit displacement, addr = An + disp. */
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

/* MOVE.L Dn, d(An): store to An + displacement. */
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

/* MOVE.L (An), Dn: load from memory at address in An. EA: dest=Dn (mode 0) in bits 11-6, source=(An) (mode 2) in bits 5-0.
 * Dest reg = (op >> 6) & 7, addr reg = op & 7. Sets N, Z; clears V, C. */
static void op_move_l_an_dn(uint16_t op)
{
    int dest_reg = (op >> 6) & 7;
    int addr_reg = op & 7;
    uint32_t addr = cpu.a[addr_reg];
    uint32_t val = mem_read32(addr);

    cpu.d[dest_reg] = val;
    set_nz_from_val(val, 4);
}

/* MOVE.L Dn, (An): store to memory at address in An. EA: source=Dn (mode 0), dest=(An) (mode 2).
 * Dest EA in bits 11-6: mode in 11-9, reg in 8-6. Source reg in bits 2-0. */
static void op_move_l_dn_an(uint16_t op)
{
    int source_reg = op & 7;
    int addr_reg = (op >> 6) & 7;
    uint32_t addr = cpu.a[addr_reg];
    uint32_t val = cpu.d[source_reg];

    mem_write32(addr, val);
    set_nz_from_val(val, 4);
}

static void op_moveq(uint16_t op)
{
    /* MOVEQ #<data>, Dn: 0111 0nnn 0ddd dddd
     * dest_reg = (op >> 9) & 7
     * imm = sign-extended (op & 0xFF)
     * Sets N, Z; clears V, C; X unchanged
     */
    int dest_reg = (op >> 9) & 7;
    int32_t imm = (int8_t)(op & 0xFF);
    uint32_t result = (uint32_t)imm;

    cpu.d[dest_reg] = result;
    set_nz_from_val(result, 4);
}

static void op_add_l_dn_dn(uint16_t op)
{
    /* ADD.L Ds, Dd: Dd = Dd + Ds
     * Format: high byte 0xD0+8*d, low byte 0x80+s
     * dest_reg = ((op >> 8) - 0xD0) >> 3
     * source_reg = (op & 0xFF) - 0x80
     * Sets N, Z, V, C
     */
    int dest_reg = ((op >> 8) - 0xD0) >> 3;
    int source_reg = (op & 0xFF) - 0x80;

    uint32_t dest_val = cpu.d[dest_reg];
    uint32_t source_val = cpu.d[source_reg];
    uint32_t result = dest_val + source_val;

    cpu.d[dest_reg] = result;
    set_nzvc_add(result, dest_val, source_val);
}

static void op_sub_l_dn_dn(uint16_t op)
{
    /* SUB.L Ds, Dd: Dd = Dd - Ds
     * Format: high byte 0x90+8*dest, low byte 0x80+source (same as ADD, different base)
     * C = borrow (set when dest_val < source_val, i.e. unsigned underflow)
     */
    int dest_reg = ((op >> 8) - 0x90) >> 3;
    int source_reg = (op & 0xFF) - 0x80;

    uint32_t dest_val = cpu.d[dest_reg];
    uint32_t source_val = cpu.d[source_reg];
    uint32_t result = dest_val - source_val;

    cpu.d[dest_reg] = result;
    set_nzvc_sub(result, dest_val, source_val);
}

static void op_cmp_l_dn_dn(uint16_t op)
{
    /* CMP.L Ds, Dd: compute Dd - Ds, set flags, don't store
     * Format: high byte 0xB0+8*dest, low byte 0x80+source (same as SUB)
     */
    int dest_reg = ((op >> 8) - 0xB0) >> 3;
    int source_reg = (op & 0xFF) - 0x80;

    uint32_t dest_val = cpu.d[dest_reg];
    uint32_t source_val = cpu.d[source_reg];
    uint32_t result = dest_val - source_val;

    /* No store - only set flags */
    set_nzvc_sub(result, dest_val, source_val);
}

/* Bcc: Branch on condition. 0x6Cxx where C=condition (bits 11-8), xx=8-bit disp.
 * When disp byte is 0, fetch 16-bit extension word.
 * Condition 0 = BRA (always). BNE=0x66, BEQ=0x67.
 */
static int bcc_condition_met(uint8_t cond)
{
    uint8_t n = (cpu.sr & SR_N) ? 1 : 0;
    uint8_t z = (cpu.sr & SR_Z) ? 1 : 0;
    uint8_t v = (cpu.sr & SR_V) ? 1 : 0;
    uint8_t c = (cpu.sr & SR_C) ? 1 : 0;

    switch (cond) {
        case 0x0: return 1;           /* BRA - always */
        case 0x1: return 1;           /* BSR - handled separately in op_bcc */
        case 0x2: return !c && !z;     /* BHI */
        case 0x3: return c || z;      /* BLS */
        case 0x4: return !c;          /* BCC/BHS */
        case 0x5: return c;          /* BCS/BLO */
        case 0x6: return !z;         /* BNE */
        case 0x7: return z;          /* BEQ */
        case 0x8: return !v;          /* BVC */
        case 0x9: return v;          /* BVS */
        case 0xA: return !n;          /* BPL */
        case 0xB: return n;          /* BMI */
        case 0xC: return (n && v) || (!n && !v);  /* BGE */
        case 0xD: return (n && !v) || (!n && v);   /* BLT */
        case 0xE: return (n && v && !z) || (!n && !v && !z);  /* BGT */
        case 0xF: return z || (n && !v) || (!n && v);        /* BLE */
        default: return 0;
    }
}

static void op_bcc(uint16_t op)
{
    uint8_t cond = (op >> 8) & 0x0F;
    int32_t disp;

    if ((op & 0xFF) != 0) {
        disp = (int8_t)(op & 0xFF);  /* 8-bit displacement */
    } else {
        disp = (int16_t)fetch16();     /* 16-bit displacement */
    }

    if (cond == 0x1) {
        /* BSR: push return address (current PC) to stack, then branch */
        cpu.a[7] -= 4;
        mem_write32(cpu.a[7], cpu.pc);
        cpu.pc += disp;
    } else if (bcc_condition_met(cond)) {
        cpu.pc += disp;
    }
}

static void op_unimplemented(uint16_t op)
{
    fprintf(stderr, "Unimplemented opcode: 0x%04X at PC=0x%08X\n", op, cpu.pc - 2);
    cpu.halted = 1;
}

/* Family dispatchers: secondary dispatch for opcode ranges that need it. */

static void dispatch_move_b(uint16_t op)
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

static void dispatch_move_l(uint16_t op)
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

static void dispatch_move_w(uint16_t op)
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

/* 0x4xxx: NOP, RTS, and future (JSR, TRAP, etc.) */
static void dispatch_4xxx(uint16_t op)
{
    if (op == 0x4E71) { op_nop(op); return; }
    if (op == 0x4E75) { op_rts(op); return; }
    op_unimplemented(op);
}

/* 0x9xxx: SUB.L Dn, Dn */
static void dispatch_9xxx(uint16_t op)
{
    if ((op & 0xF0F8) == 0x9080) { op_sub_l_dn_dn(op); return; }
    op_unimplemented(op);
}

/* 0xBxxx: CMP.L Dn, Dn */
static void dispatch_Bxxx(uint16_t op)
{
    if ((op & 0xF0F8) == 0xB080) { op_cmp_l_dn_dn(op); return; }
    op_unimplemented(op);
}

/* 0xDxxx: ADD.L Dn, Dn */
static void dispatch_Dxxx(uint16_t op)
{
    if ((op & 0xF0F8) == 0xD080) { op_add_l_dn_dn(op); return; }
    op_unimplemented(op);
}

/* 0xExxx: ADD.L Dn, Dn */
static void dispatch_Exxx(uint16_t op)
{
    if ((op & 0xF0F8) == 0xE080) { op_add_l_dn_dn(op); return; }
    op_unimplemented(op);
}

/* 0xFxxx: ADD.L Dn, Dn */
static void dispatch_Fxxx(uint16_t op)
{
    if ((op & 0xF0F8) == 0xF080) { op_add_l_dn_dn(op); return; }
    op_unimplemented(op);
}

/* Top-nibble dispatch table. Index = op >> 12. */
typedef void (*op_handler_fn)(uint16_t op);

static const op_handler_fn dispatch_top[16] = {
    [0x0] = op_unimplemented,
    [0x1] = dispatch_move_b,
    [0x2] = dispatch_move_l,
    [0x3] = dispatch_move_w,
    [0x4] = dispatch_4xxx,
    [0x5] = op_unimplemented,
    [0x6] = op_bcc,
    [0x7] = op_moveq,
    [0x8] = op_unimplemented,
    [0x9] = dispatch_9xxx,
    [0xA] = op_unimplemented,
    [0xB] = dispatch_Bxxx,
    [0xC] = op_unimplemented,
    [0xD] = dispatch_Dxxx,
    [0xE] = dispatch_Exxx,
    [0xF] = dispatch_Fxxx,
};

static void execute(uint16_t op)
{
    dispatch_top[op >> 12](op);
}

int cpu_step(void)
{
    if (cpu.halted)
        return 0;

    uint16_t op = fetch16();
    execute(op);

    /* Return 4 cycles as placeholder (NOP takes 4 cycles on 68K) */
    return 4;
}

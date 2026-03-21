/*
 * Minimal Z80 CPU emulator for Genesis sound drivers.
 *
 * Implements enough of the Z80 instruction set to execute common
 * sound driver init sequences (handshake writes, YM2612 register
 * setup, basic control flow).  Not cycle-accurate; just enough to
 * get the Z80 past its init and into its idle loop.
 */

#include "z80.h"
#include <string.h>
#include <stdio.h>

/* ------------------------------------------------------------------ */
/*  Z80 RAM (8 KB, visible to 68K via bus at $A00000)                  */
/* ------------------------------------------------------------------ */

#define Z80_RAM_SIZE  0x2000
static uint8_t z80_ram[Z80_RAM_SIZE];

/* ------------------------------------------------------------------ */
/*  Z80 flags                                                          */
/* ------------------------------------------------------------------ */

#define ZF_C   0x01
#define ZF_N   0x02
#define ZF_PV  0x04
#define ZF_H   0x10
#define ZF_Z   0x40
#define ZF_S   0x80

/* ------------------------------------------------------------------ */
/*  Z80 CPU state                                                      */
/* ------------------------------------------------------------------ */

static struct {
    uint8_t  a, f;
    uint8_t  b, c, d, e, h, l;
    uint16_t sp, pc;
    uint16_t ix, iy;
    uint8_t  iff1, iff2;
    uint8_t  im;
    int      halted;
    int      running;
} z;

/* ------------------------------------------------------------------ */
/*  Register pair helpers                                               */
/* ------------------------------------------------------------------ */

static inline uint16_t rp_bc(void) { return ((uint16_t)z.b << 8) | z.c; }
static inline uint16_t rp_de(void) { return ((uint16_t)z.d << 8) | z.e; }
static inline uint16_t rp_hl(void) { return ((uint16_t)z.h << 8) | z.l; }

static inline void set_bc(uint16_t v) { z.b = v >> 8; z.c = v & 0xFF; }
static inline void set_de(uint16_t v) { z.d = v >> 8; z.e = v & 0xFF; }
static inline void set_hl(uint16_t v) { z.h = v >> 8; z.l = v & 0xFF; }

/* ------------------------------------------------------------------ */
/*  Z80 memory access                                                  */
/* ------------------------------------------------------------------ */

static uint8_t z80_read(uint16_t addr)
{
    if (addr < 0x2000) return z80_ram[addr];
    if (addr < 0x4000) return z80_ram[addr & 0x1FFF];
    if (addr >= 0x4000 && addr <= 0x5FFF) return 0x00; /* YM2612: not busy */
    return 0xFF;
}

static void z80_write(uint16_t addr, uint8_t val)
{
    if (addr < 0x2000) { z80_ram[addr] = val; return; }
    if (addr < 0x4000) { z80_ram[addr & 0x1FFF] = val; return; }
    /* YM2612 / PSG / bank writes: silently ignore */
}

static uint8_t fetch(void) { return z80_read(z.pc++); }

static uint16_t fetch16(void)
{
    uint8_t lo = fetch();
    uint8_t hi = fetch();
    return ((uint16_t)hi << 8) | lo;
}

/* ------------------------------------------------------------------ */
/*  Flag computation helpers                                           */
/* ------------------------------------------------------------------ */

static const uint8_t parity_table[256] = {
    1,0,0,1,0,1,1,0,0,1,1,0,1,0,0,1,0,1,1,0,1,0,0,1,1,0,0,1,0,1,1,0,
    0,1,1,0,1,0,0,1,1,0,0,1,0,1,1,0,1,0,0,1,0,1,1,0,0,1,1,0,1,0,0,1,
    0,1,1,0,1,0,0,1,1,0,0,1,0,1,1,0,1,0,0,1,0,1,1,0,0,1,1,0,1,0,0,1,
    1,0,0,1,0,1,1,0,0,1,1,0,1,0,0,1,0,1,1,0,1,0,0,1,1,0,0,1,0,1,1,0,
    0,1,1,0,1,0,0,1,1,0,0,1,0,1,1,0,1,0,0,1,0,1,1,0,0,1,1,0,1,0,0,1,
    1,0,0,1,0,1,1,0,0,1,1,0,1,0,0,1,0,1,1,0,1,0,0,1,1,0,0,1,0,1,1,0,
    1,0,0,1,0,1,1,0,0,1,1,0,1,0,0,1,0,1,1,0,1,0,0,1,1,0,0,1,0,1,1,0,
    0,1,1,0,1,0,0,1,1,0,0,1,0,1,1,0,1,0,0,1,0,1,1,0,0,1,1,0,1,0,0,1,
};

static uint8_t sz_flags(uint8_t v)
{
    return (v & ZF_S) | (v ? 0 : ZF_Z) | (parity_table[v] ? ZF_PV : 0);
}

/* ------------------------------------------------------------------ */
/*  8-bit register decode (bits 5-3 or 2-0 of opcode)                  */
/* ------------------------------------------------------------------ */

static uint8_t read_r(int r)
{
    switch (r) {
    case 0: return z.b;
    case 1: return z.c;
    case 2: return z.d;
    case 3: return z.e;
    case 4: return z.h;
    case 5: return z.l;
    case 6: return z80_read(rp_hl());
    case 7: return z.a;
    }
    return 0;
}

static void write_r(int r, uint8_t v)
{
    switch (r) {
    case 0: z.b = v; break;
    case 1: z.c = v; break;
    case 2: z.d = v; break;
    case 3: z.e = v; break;
    case 4: z.h = v; break;
    case 5: z.l = v; break;
    case 6: z80_write(rp_hl(), v); break;
    case 7: z.a = v; break;
    }
}

/* ------------------------------------------------------------------ */
/*  ALU operations on A                                                */
/* ------------------------------------------------------------------ */

static void alu_add(uint8_t v, int carry)
{
    int c = carry ? (z.f & ZF_C) : 0;
    int r = z.a + v + c;
    int hc = (z.a & 0x0F) + (v & 0x0F) + c;
    z.f = (r & ZF_S) | ((r & 0xFF) ? 0 : ZF_Z)
        | (hc > 0x0F ? ZF_H : 0)
        | ((((z.a ^ ~v) & (z.a ^ r)) & 0x80) ? ZF_PV : 0)
        | (r > 0xFF ? ZF_C : 0);
    z.a = r & 0xFF;
}

static void alu_sub(uint8_t v, int carry)
{
    int c = carry ? (z.f & ZF_C) : 0;
    int r = z.a - v - c;
    int hc = (z.a & 0x0F) - (v & 0x0F) - c;
    z.f = (r & ZF_S) | ((r & 0xFF) ? 0 : ZF_Z)
        | ZF_N
        | (hc < 0 ? ZF_H : 0)
        | ((((z.a ^ v) & (z.a ^ r)) & 0x80) ? ZF_PV : 0)
        | (r < 0 ? ZF_C : 0);
    z.a = r & 0xFF;
}

static void alu_and(uint8_t v)
{
    z.a &= v;
    z.f = sz_flags(z.a) | ZF_H;
}

static void alu_xor(uint8_t v)
{
    z.a ^= v;
    z.f = sz_flags(z.a);
}

static void alu_or(uint8_t v)
{
    z.a |= v;
    z.f = sz_flags(z.a);
}

static void alu_cp(uint8_t v)
{
    uint8_t old_a = z.a;
    alu_sub(v, 0);
    z.a = old_a;
}

static void do_alu(int op, uint8_t v)
{
    switch (op) {
    case 0: alu_add(v, 0); break;
    case 1: alu_add(v, 1); break;
    case 2: alu_sub(v, 0); break;
    case 3: alu_sub(v, 1); break;
    case 4: alu_and(v); break;
    case 5: alu_xor(v); break;
    case 6: alu_or(v); break;
    case 7: alu_cp(v); break;
    }
}

/* ------------------------------------------------------------------ */
/*  Condition code evaluation                                          */
/* ------------------------------------------------------------------ */

static int eval_cc(int cc)
{
    switch (cc) {
    case 0: return !(z.f & ZF_Z);   /* NZ */
    case 1: return  (z.f & ZF_Z);   /* Z  */
    case 2: return !(z.f & ZF_C);   /* NC */
    case 3: return  (z.f & ZF_C);   /* C  */
    case 4: return !(z.f & ZF_PV);  /* PO */
    case 5: return  (z.f & ZF_PV);  /* PE */
    case 6: return !(z.f & ZF_S);   /* P  */
    case 7: return  (z.f & ZF_S);   /* M  */
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  16-bit register pair decode (bits 5-4)                             */
/* ------------------------------------------------------------------ */

static uint16_t read_rp(int p)
{
    switch (p) {
    case 0: return rp_bc();
    case 1: return rp_de();
    case 2: return rp_hl();
    case 3: return z.sp;
    }
    return 0;
}


/* ------------------------------------------------------------------ */
/*  Stack helpers                                                      */
/* ------------------------------------------------------------------ */

static void push16(uint16_t v)
{
    z.sp -= 2;
    z80_write(z.sp, v & 0xFF);
    z80_write(z.sp + 1, v >> 8);
}

static uint16_t pop16(void)
{
    uint8_t lo = z80_read(z.sp);
    uint8_t hi = z80_read(z.sp + 1);
    z.sp += 2;
    return ((uint16_t)hi << 8) | lo;
}

/* ------------------------------------------------------------------ */
/*  CB-prefixed instructions (bit ops, rotates, shifts)                */
/* ------------------------------------------------------------------ */

static int exec_cb(void)
{
    uint8_t op = fetch();
    int r = op & 7;
    int b = (op >> 3) & 7;
    uint8_t v = read_r(r);

    if (op < 0x40) {
        /* Rotates and shifts */
        uint8_t result;
        switch (b) {
        case 0: /* RLC */
            result = (v << 1) | (v >> 7);
            z.f = (result & ZF_S) | (result ? 0 : ZF_Z)
                | (parity_table[result] ? ZF_PV : 0) | (v >> 7);
            break;
        case 1: /* RRC */
            result = (v >> 1) | (v << 7);
            z.f = (result & ZF_S) | (result ? 0 : ZF_Z)
                | (parity_table[result] ? ZF_PV : 0) | (v & 1);
            break;
        case 2: /* RL */
            result = (v << 1) | (z.f & ZF_C);
            z.f = (result & ZF_S) | (result ? 0 : ZF_Z)
                | (parity_table[result] ? ZF_PV : 0) | (v >> 7);
            break;
        case 3: /* RR */
            result = (v >> 1) | ((z.f & ZF_C) << 7);
            z.f = (result & ZF_S) | (result ? 0 : ZF_Z)
                | (parity_table[result] ? ZF_PV : 0) | (v & 1);
            break;
        case 4: /* SLA */
            result = v << 1;
            z.f = (result & ZF_S) | (result ? 0 : ZF_Z)
                | (parity_table[result] ? ZF_PV : 0) | (v >> 7);
            break;
        case 5: /* SRA */
            result = (v >> 1) | (v & 0x80);
            z.f = (result & ZF_S) | (result ? 0 : ZF_Z)
                | (parity_table[result] ? ZF_PV : 0) | (v & 1);
            break;
        case 6: /* SLL (undocumented) */
            result = (v << 1) | 1;
            z.f = (result & ZF_S) | (result ? 0 : ZF_Z)
                | (parity_table[result] ? ZF_PV : 0) | (v >> 7);
            break;
        default: /* SRL */
            result = v >> 1;
            z.f = (result & ZF_S) | (result ? 0 : ZF_Z)
                | (parity_table[result] ? ZF_PV : 0) | (v & 1);
            break;
        }
        write_r(r, result);
    } else if (op < 0x80) {
        /* BIT b,r */
        uint8_t mask = 1 << b;
        z.f = (z.f & ZF_C) | ZF_H | ((v & mask) ? 0 : ZF_Z);
        if (b == 7 && (v & mask)) z.f |= ZF_S;
    } else if (op < 0xC0) {
        /* RES b,r */
        write_r(r, v & ~(1 << b));
    } else {
        /* SET b,r */
        write_r(r, v | (1 << b));
    }

    return (r == 6) ? 15 : 8;
}

/* ------------------------------------------------------------------ */
/*  DD/FD CB prefix: bit ops on (IX+d) / (IY+d)                       */
/* ------------------------------------------------------------------ */

static int exec_ddfd_cb(uint16_t idx)
{
    int8_t d = (int8_t)fetch();
    uint8_t op = fetch();
    uint16_t addr = idx + d;
    uint8_t v = z80_read(addr);
    int b = (op >> 3) & 7;

    if (op >= 0x40 && op <= 0x7F) {
        /* BIT b,(IX/IY+d) */
        uint8_t mask = 1 << b;
        z.f = (z.f & ZF_C) | ZF_H | ((v & mask) ? 0 : ZF_Z);
        if (b == 7 && (v & mask)) z.f |= ZF_S;
    } else if (op >= 0x80 && op <= 0xBF) {
        /* RES b,(IX/IY+d) */
        z80_write(addr, v & ~(1 << b));
    } else if (op >= 0xC0) {
        /* SET b,(IX/IY+d) */
        z80_write(addr, v | (1 << b));
    } else {
        /* Rotate/shift on (IX/IY+d): handle the most common */
        uint8_t result = 0;
        switch (b) {
        case 0: result = (v << 1) | (v >> 7); z.f = (v >> 7); break;
        case 1: result = (v >> 1) | (v << 7); z.f = (v & 1); break;
        case 2: result = (v << 1) | (z.f & ZF_C); z.f = (v >> 7); break;
        case 3: result = (v >> 1) | ((z.f & ZF_C) << 7); z.f = (v & 1); break;
        case 4: result = v << 1; z.f = (v >> 7); break;
        case 5: result = (v >> 1) | (v & 0x80); z.f = (v & 1); break;
        case 6: result = (v << 1) | 1; z.f = (v >> 7); break;
        default: result = v >> 1; z.f = (v & 1); break;
        }
        z.f |= (result & ZF_S) | (result ? 0 : ZF_Z)
             | (parity_table[result] ? ZF_PV : 0);
        z80_write(addr, result);
    }

    return 23;
}

/* ------------------------------------------------------------------ */
/*  DD/FD prefix: IX/IY instructions                                   */
/* ------------------------------------------------------------------ */

static int exec_ddfd(uint16_t *idx)
{
    uint8_t op = fetch();

    switch (op) {
    case 0x09: { /* ADD IX/IY, BC */
        uint32_t r = *idx + rp_bc();
        z.f = (z.f & (ZF_S|ZF_Z|ZF_PV)) | (r > 0xFFFF ? ZF_C : 0);
        *idx = r & 0xFFFF;
        return 15;
    }
    case 0x19: { /* ADD IX/IY, DE */
        uint32_t r = *idx + rp_de();
        z.f = (z.f & (ZF_S|ZF_Z|ZF_PV)) | (r > 0xFFFF ? ZF_C : 0);
        *idx = r & 0xFFFF;
        return 15;
    }
    case 0x21: /* LD IX/IY, nn */
        *idx = fetch16();
        return 14;
    case 0x22: { /* LD (nn), IX/IY */
        uint16_t addr = fetch16();
        z80_write(addr, *idx & 0xFF);
        z80_write(addr + 1, *idx >> 8);
        return 20;
    }
    case 0x23: /* INC IX/IY */
        (*idx)++;
        return 10;
    case 0x2A: { /* LD IX/IY, (nn) */
        uint16_t addr = fetch16();
        *idx = z80_read(addr) | ((uint16_t)z80_read(addr + 1) << 8);
        return 20;
    }
    case 0x2B: /* DEC IX/IY */
        (*idx)--;
        return 10;
    case 0x29: { /* ADD IX/IY, IX/IY */
        uint32_t r = *idx + *idx;
        z.f = (z.f & (ZF_S|ZF_Z|ZF_PV)) | (r > 0xFFFF ? ZF_C : 0);
        *idx = r & 0xFFFF;
        return 15;
    }
    case 0x34: { /* INC (IX/IY+d) */
        int8_t d = (int8_t)fetch();
        uint16_t addr = *idx + d;
        uint8_t v = z80_read(addr);
        uint8_t r = v + 1;
        z.f = (z.f & ZF_C) | (r & ZF_S) | (r ? 0 : ZF_Z)
            | ((v & 0x0F) == 0x0F ? ZF_H : 0)
            | (v == 0x7F ? ZF_PV : 0);
        z80_write(addr, r);
        return 23;
    }
    case 0x35: { /* DEC (IX/IY+d) */
        int8_t d = (int8_t)fetch();
        uint16_t addr = *idx + d;
        uint8_t v = z80_read(addr);
        uint8_t r = v - 1;
        z.f = (z.f & ZF_C) | ZF_N | (r & ZF_S) | (r ? 0 : ZF_Z)
            | ((v & 0x0F) == 0x00 ? ZF_H : 0)
            | (v == 0x80 ? ZF_PV : 0);
        z80_write(addr, r);
        return 23;
    }
    case 0x36: { /* LD (IX/IY+d), n */
        int8_t d = (int8_t)fetch();
        uint8_t n = fetch();
        z80_write(*idx + d, n);
        return 19;
    }
    case 0x39: { /* ADD IX/IY, SP */
        uint32_t r = *idx + z.sp;
        z.f = (z.f & (ZF_S|ZF_Z|ZF_PV)) | (r > 0xFFFF ? ZF_C : 0);
        *idx = r & 0xFFFF;
        return 15;
    }
    case 0xCB:
        return exec_ddfd_cb(*idx);
    case 0xE1: /* POP IX/IY */
        *idx = pop16();
        return 14;
    case 0xE3: { /* EX (SP), IX/IY */
        uint16_t v = z80_read(z.sp) | ((uint16_t)z80_read(z.sp + 1) << 8);
        z80_write(z.sp, *idx & 0xFF);
        z80_write(z.sp + 1, *idx >> 8);
        *idx = v;
        return 23;
    }
    case 0xE5: /* PUSH IX/IY */
        push16(*idx);
        return 15;
    case 0xE9: /* JP (IX/IY) */
        z.pc = *idx;
        return 8;
    case 0xF9: /* LD SP, IX/IY */
        z.sp = *idx;
        return 10;
    default:
        /* LD r, (IX/IY+d) or LD (IX/IY+d), r */
        if (op >= 0x46 && op <= 0x7E && (op & 7) == 6 && op != 0x76) {
            int8_t d = (int8_t)fetch();
            int dst = (op >> 3) & 7;
            write_r(dst, z80_read(*idx + d));
            return 19;
        }
        if (op >= 0x70 && op <= 0x77 && op != 0x76) {
            int8_t d = (int8_t)fetch();
            int src = op & 7;
            z80_write(*idx + d, read_r(src));
            return 19;
        }
        /* ALU A, (IX/IY+d) */
        if (op >= 0x86 && op <= 0xBE && (op & 7) == 6) {
            int8_t d = (int8_t)fetch();
            do_alu((op >> 3) & 7, z80_read(*idx + d));
            return 19;
        }
        break;
    }

    return 4; /* unknown DD/FD instruction, treat as NOP */
}

/* ------------------------------------------------------------------ */
/*  ED prefix                                                          */
/* ------------------------------------------------------------------ */

static int exec_ed(void)
{
    uint8_t op = fetch();

    switch (op) {
    case 0x46: z.im = 0; return 8;  /* IM 0 */
    case 0x56: z.im = 1; return 8;  /* IM 1 */
    case 0x5E: z.im = 2; return 8;  /* IM 2 */

    case 0x47: /* LD I, A (stub) */  return 9;
    case 0x4F: /* LD R, A (stub) */  return 9;
    case 0x57: /* LD A, I (stub) */  z.a = 0; z.f = (z.f & ZF_C) | (z.a ? 0 : ZF_Z); return 9;
    case 0x5F: /* LD A, R (stub) */  z.a = 0; z.f = (z.f & ZF_C) | (z.a ? 0 : ZF_Z); return 9;

    case 0x43: { /* LD (nn), BC */
        uint16_t addr = fetch16();
        z80_write(addr, z.c);
        z80_write(addr + 1, z.b);
        return 20;
    }
    case 0x4B: { /* LD BC, (nn) */
        uint16_t addr = fetch16();
        z.c = z80_read(addr);
        z.b = z80_read(addr + 1);
        return 20;
    }
    case 0x53: { /* LD (nn), DE */
        uint16_t addr = fetch16();
        z80_write(addr, z.e);
        z80_write(addr + 1, z.d);
        return 20;
    }
    case 0x5B: { /* LD DE, (nn) */
        uint16_t addr = fetch16();
        z.e = z80_read(addr);
        z.d = z80_read(addr + 1);
        return 20;
    }
    case 0x63: { /* LD (nn), HL (ED version) */
        uint16_t addr = fetch16();
        z80_write(addr, z.l);
        z80_write(addr + 1, z.h);
        return 20;
    }
    case 0x6B: { /* LD HL, (nn) (ED version) */
        uint16_t addr = fetch16();
        z.l = z80_read(addr);
        z.h = z80_read(addr + 1);
        return 20;
    }
    case 0x73: { /* LD (nn), SP */
        uint16_t addr = fetch16();
        z80_write(addr, z.sp & 0xFF);
        z80_write(addr + 1, z.sp >> 8);
        return 20;
    }
    case 0x7B: { /* LD SP, (nn) */
        uint16_t addr = fetch16();
        z.sp = z80_read(addr) | ((uint16_t)z80_read(addr + 1) << 8);
        return 20;
    }

    case 0x44: case 0x4C: case 0x54: case 0x5C:
    case 0x64: case 0x6C: case 0x74: case 0x7C: {
        /* NEG */
        uint8_t old = z.a;
        z.a = 0 - z.a;
        z.f = ZF_N | (z.a & ZF_S) | (z.a ? 0 : ZF_Z)
            | (old ? ZF_C : 0) | (old == 0x80 ? ZF_PV : 0)
            | ((0 ^ old ^ z.a) & ZF_H);
        return 8;
    }

    case 0x45: case 0x4D: case 0x55: case 0x5D:
    case 0x65: case 0x6D: case 0x75: case 0x7D:
        /* RETN / RETI */
        z.pc = pop16();
        z.iff1 = z.iff2;
        return 14;

    case 0xA0: { /* LDI */
        uint8_t v = z80_read(rp_hl());
        z80_write(rp_de(), v);
        set_hl(rp_hl() + 1);
        set_de(rp_de() + 1);
        set_bc(rp_bc() - 1);
        z.f = (z.f & (ZF_S|ZF_Z|ZF_C)) | (rp_bc() ? ZF_PV : 0);
        return 16;
    }
    case 0xA8: { /* LDD */
        uint8_t v = z80_read(rp_hl());
        z80_write(rp_de(), v);
        set_hl(rp_hl() - 1);
        set_de(rp_de() - 1);
        set_bc(rp_bc() - 1);
        z.f = (z.f & (ZF_S|ZF_Z|ZF_C)) | (rp_bc() ? ZF_PV : 0);
        return 16;
    }
    case 0xB0: { /* LDIR */
        uint8_t v = z80_read(rp_hl());
        z80_write(rp_de(), v);
        set_hl(rp_hl() + 1);
        set_de(rp_de() + 1);
        set_bc(rp_bc() - 1);
        z.f = (z.f & (ZF_S|ZF_Z|ZF_C));
        if (rp_bc()) { z.pc -= 2; z.f |= ZF_PV; return 21; }
        return 16;
    }
    case 0xB8: { /* LDDR */
        uint8_t v = z80_read(rp_hl());
        z80_write(rp_de(), v);
        set_hl(rp_hl() - 1);
        set_de(rp_de() - 1);
        set_bc(rp_bc() - 1);
        z.f = (z.f & (ZF_S|ZF_Z|ZF_C));
        if (rp_bc()) { z.pc -= 2; z.f |= ZF_PV; return 21; }
        return 16;
    }

    case 0xA1: { /* CPI */
        uint8_t v = z80_read(rp_hl());
        int r = z.a - v;
        set_hl(rp_hl() + 1);
        set_bc(rp_bc() - 1);
        z.f = (z.f & ZF_C) | ZF_N | (r & ZF_S) | ((r & 0xFF) ? 0 : ZF_Z)
            | ((z.a ^ v ^ r) & ZF_H) | (rp_bc() ? ZF_PV : 0);
        return 16;
    }
    case 0xB1: { /* CPIR */
        uint8_t v = z80_read(rp_hl());
        int r = z.a - v;
        set_hl(rp_hl() + 1);
        set_bc(rp_bc() - 1);
        z.f = (z.f & ZF_C) | ZF_N | (r & ZF_S) | ((r & 0xFF) ? 0 : ZF_Z)
            | ((z.a ^ v ^ r) & ZF_H) | (rp_bc() ? ZF_PV : 0);
        if (rp_bc() && (r & 0xFF)) { z.pc -= 2; return 21; }
        return 16;
    }

    case 0x40: case 0x48: case 0x50: case 0x58:
    case 0x60: case 0x68: case 0x78: {
        /* IN r, (C) - stub: read 0 */
        int r = (op >> 3) & 7;
        write_r(r, 0);
        z.f = (z.f & ZF_C) | sz_flags(0);
        return 12;
    }
    case 0x41: case 0x49: case 0x51: case 0x59:
    case 0x61: case 0x69: case 0x79:
        /* OUT (C), r - stub: ignore */
        return 12;

    default:
        return 8; /* unknown ED, treat as NOP pair */
    }
}

/* ------------------------------------------------------------------ */
/*  Main instruction dispatch                                          */
/* ------------------------------------------------------------------ */

int z80_step(void)
{
    if (!z.running || z.halted)
        return 0;

    uint8_t op = fetch();

    /* LD r, r' block: 0x40-0x7F (except 0x76 = HALT) */
    if (op >= 0x40 && op <= 0x7F) {
        if (op == 0x76) {
            z.halted = 1;
            return 4;
        }
        write_r((op >> 3) & 7, read_r(op & 7));
        return (op & 7) == 6 || ((op >> 3) & 7) == 6 ? 7 : 4;
    }

    /* ALU A, r block: 0x80-0xBF */
    if (op >= 0x80 && op <= 0xBF) {
        do_alu((op >> 3) & 7, read_r(op & 7));
        return (op & 7) == 6 ? 7 : 4;
    }

    switch (op) {
    case 0x00: return 4;  /* NOP */

    /* LD rp, nn */
    case 0x01: set_bc(fetch16()); return 10;
    case 0x11: set_de(fetch16()); return 10;
    case 0x21: set_hl(fetch16()); return 10;
    case 0x31: z.sp = fetch16(); return 10;

    /* LD (rp), A */
    case 0x02: z80_write(rp_bc(), z.a); return 7;
    case 0x12: z80_write(rp_de(), z.a); return 7;

    /* LD A, (rp) */
    case 0x0A: z.a = z80_read(rp_bc()); return 7;
    case 0x1A: z.a = z80_read(rp_de()); return 7;

    /* LD (nn), HL */
    case 0x22: {
        uint16_t addr = fetch16();
        z80_write(addr, z.l);
        z80_write(addr + 1, z.h);
        return 16;
    }
    /* LD HL, (nn) */
    case 0x2A: {
        uint16_t addr = fetch16();
        z.l = z80_read(addr);
        z.h = z80_read(addr + 1);
        return 16;
    }
    /* LD (nn), A */
    case 0x32: {
        uint16_t addr = fetch16();
        z80_write(addr, z.a);
        return 13;
    }
    /* LD A, (nn) */
    case 0x3A: {
        uint16_t addr = fetch16();
        z.a = z80_read(addr);
        return 13;
    }

    /* INC rp */
    case 0x03: set_bc(rp_bc() + 1); return 6;
    case 0x13: set_de(rp_de() + 1); return 6;
    case 0x23: set_hl(rp_hl() + 1); return 6;
    case 0x33: z.sp++; return 6;

    /* DEC rp */
    case 0x0B: set_bc(rp_bc() - 1); return 6;
    case 0x1B: set_de(rp_de() - 1); return 6;
    case 0x2B: set_hl(rp_hl() - 1); return 6;
    case 0x3B: z.sp--; return 6;

    /* ADD HL, rp */
    case 0x09: case 0x19: case 0x29: case 0x39: {
        uint32_t r = rp_hl() + read_rp((op >> 4) & 3);
        z.f = (z.f & (ZF_S|ZF_Z|ZF_PV)) | (r > 0xFFFF ? ZF_C : 0);
        set_hl(r & 0xFFFF);
        return 11;
    }

    /* INC r */
    case 0x04: case 0x0C: case 0x14: case 0x1C:
    case 0x24: case 0x2C: case 0x34: case 0x3C: {
        int r = (op >> 3) & 7;
        uint8_t v = read_r(r);
        uint8_t res = v + 1;
        z.f = (z.f & ZF_C) | (res & ZF_S) | (res ? 0 : ZF_Z)
            | ((v & 0x0F) == 0x0F ? ZF_H : 0)
            | (v == 0x7F ? ZF_PV : 0);
        write_r(r, res);
        return r == 6 ? 11 : 4;
    }

    /* DEC r */
    case 0x05: case 0x0D: case 0x15: case 0x1D:
    case 0x25: case 0x2D: case 0x35: case 0x3D: {
        int r = (op >> 3) & 7;
        uint8_t v = read_r(r);
        uint8_t res = v - 1;
        z.f = (z.f & ZF_C) | ZF_N | (res & ZF_S) | (res ? 0 : ZF_Z)
            | ((v & 0x0F) == 0x00 ? ZF_H : 0)
            | (v == 0x80 ? ZF_PV : 0);
        write_r(r, res);
        return r == 6 ? 11 : 4;
    }

    /* LD r, n */
    case 0x06: case 0x0E: case 0x16: case 0x1E:
    case 0x26: case 0x2E: case 0x36: case 0x3E: {
        int r = (op >> 3) & 7;
        write_r(r, fetch());
        return r == 6 ? 10 : 7;
    }

    /* Rotate A instructions */
    case 0x07: { /* RLCA */
        uint8_t c = z.a >> 7;
        z.a = (z.a << 1) | c;
        z.f = (z.f & (ZF_S|ZF_Z|ZF_PV)) | c;
        return 4;
    }
    case 0x0F: { /* RRCA */
        uint8_t c = z.a & 1;
        z.a = (z.a >> 1) | (c << 7);
        z.f = (z.f & (ZF_S|ZF_Z|ZF_PV)) | c;
        return 4;
    }
    case 0x17: { /* RLA */
        uint8_t c = z.a >> 7;
        z.a = (z.a << 1) | (z.f & ZF_C);
        z.f = (z.f & (ZF_S|ZF_Z|ZF_PV)) | c;
        return 4;
    }
    case 0x1F: { /* RRA */
        uint8_t c = z.a & 1;
        z.a = (z.a >> 1) | ((z.f & ZF_C) << 7);
        z.f = (z.f & (ZF_S|ZF_Z|ZF_PV)) | c;
        return 4;
    }

    /* DJNZ */
    case 0x10: {
        int8_t d = (int8_t)fetch();
        z.b--;
        if (z.b) { z.pc += d; return 13; }
        return 8;
    }

    /* JR */
    case 0x18: { int8_t d = (int8_t)fetch(); z.pc += d; return 12; }
    case 0x20: { /* JR NZ */
        int8_t d = (int8_t)fetch();
        if (!(z.f & ZF_Z)) { z.pc += d; return 12; }
        return 7;
    }
    case 0x28: { /* JR Z */
        int8_t d = (int8_t)fetch();
        if (z.f & ZF_Z) { z.pc += d; return 12; }
        return 7;
    }
    case 0x30: { /* JR NC */
        int8_t d = (int8_t)fetch();
        if (!(z.f & ZF_C)) { z.pc += d; return 12; }
        return 7;
    }
    case 0x38: { /* JR C */
        int8_t d = (int8_t)fetch();
        if (z.f & ZF_C) { z.pc += d; return 12; }
        return 7;
    }

    /* EX AF, AF' (stub: ignore alternate set) */
    case 0x08: return 4;
    /* EX DE, HL */
    case 0xEB: {
        uint16_t t = rp_de();
        set_de(rp_hl());
        set_hl(t);
        return 4;
    }
    /* EXX (stub: ignore alternate set) */
    case 0xD9: return 4;
    /* EX (SP), HL */
    case 0xE3: {
        uint16_t v = z80_read(z.sp) | ((uint16_t)z80_read(z.sp + 1) << 8);
        z80_write(z.sp, z.l);
        z80_write(z.sp + 1, z.h);
        set_hl(v);
        return 19;
    }

    /* JP nn */
    case 0xC3: z.pc = fetch16(); return 10;
    /* JP cc, nn */
    case 0xC2: case 0xCA: case 0xD2: case 0xDA:
    case 0xE2: case 0xEA: case 0xF2: case 0xFA: {
        uint16_t addr = fetch16();
        if (eval_cc((op >> 3) & 7)) z.pc = addr;
        return 10;
    }
    /* JP (HL) */
    case 0xE9: z.pc = rp_hl(); return 4;

    /* CALL nn */
    case 0xCD: {
        uint16_t addr = fetch16();
        push16(z.pc);
        z.pc = addr;
        return 17;
    }
    /* CALL cc, nn */
    case 0xC4: case 0xCC: case 0xD4: case 0xDC:
    case 0xE4: case 0xEC: case 0xF4: case 0xFC: {
        uint16_t addr = fetch16();
        if (eval_cc((op >> 3) & 7)) { push16(z.pc); z.pc = addr; return 17; }
        return 10;
    }

    /* RET */
    case 0xC9: z.pc = pop16(); return 10;
    /* RET cc */
    case 0xC0: case 0xC8: case 0xD0: case 0xD8:
    case 0xE0: case 0xE8: case 0xF0: case 0xF8:
        if (eval_cc((op >> 3) & 7)) { z.pc = pop16(); return 11; }
        return 5;

    /* PUSH rp */
    case 0xC5: push16(rp_bc()); return 11;
    case 0xD5: push16(rp_de()); return 11;
    case 0xE5: push16(rp_hl()); return 11;
    case 0xF5: push16(((uint16_t)z.a << 8) | z.f); return 11;

    /* POP rp */
    case 0xC1: set_bc(pop16()); return 10;
    case 0xD1: set_de(pop16()); return 10;
    case 0xE1: set_hl(pop16()); return 10;
    case 0xF1: { uint16_t v = pop16(); z.a = v >> 8; z.f = v & 0xFF; return 10; }

    /* RST */
    case 0xC7: case 0xCF: case 0xD7: case 0xDF:
    case 0xE7: case 0xEF: case 0xF7: case 0xFF:
        push16(z.pc);
        z.pc = op & 0x38;
        return 11;

    /* ALU A, n */
    case 0xC6: do_alu(0, fetch()); return 7;
    case 0xCE: do_alu(1, fetch()); return 7;
    case 0xD6: do_alu(2, fetch()); return 7;
    case 0xDE: do_alu(3, fetch()); return 7;
    case 0xE6: do_alu(4, fetch()); return 7;
    case 0xEE: do_alu(5, fetch()); return 7;
    case 0xF6: do_alu(6, fetch()); return 7;
    case 0xFE: do_alu(7, fetch()); return 7;

    /* DI / EI */
    case 0xF3: z.iff1 = z.iff2 = 0; return 4;
    case 0xFB: z.iff1 = z.iff2 = 1; return 4;

    /* LD SP, HL */
    case 0xF9: z.sp = rp_hl(); return 6;

    /* SCF / CCF / CPL / DAA */
    case 0x37: z.f = (z.f & (ZF_S|ZF_Z|ZF_PV)) | ZF_C; return 4;
    case 0x3F: z.f = (z.f & (ZF_S|ZF_Z|ZF_PV)) | ((z.f & ZF_C) ? ZF_H : 0)
                    | ((z.f & ZF_C) ? 0 : ZF_C); return 4;
    case 0x2F: z.a = ~z.a; z.f |= ZF_N | ZF_H; return 4;
    case 0x27: { /* DAA */
        int a = z.a;
        int correction = 0;
        int carry = z.f & ZF_C;
        if ((z.f & ZF_H) || (a & 0x0F) > 9) correction |= 0x06;
        if (carry || a > 0x99) { correction |= 0x60; carry = 1; }
        if (z.f & ZF_N) a -= correction; else a += correction;
        z.a = a & 0xFF;
        z.f = (z.a & ZF_S) | (z.a ? 0 : ZF_Z) | (parity_table[z.a] ? ZF_PV : 0)
            | (z.f & ZF_N) | ((z.f & ZF_H) ? ZF_H : 0) | (carry ? ZF_C : 0);
        return 4;
    }

    /* IN A, (n) */
    case 0xDB: fetch(); z.a = 0; return 11;
    /* OUT (n), A */
    case 0xD3: fetch(); return 11;

    /* Prefixes */
    case 0xCB: return exec_cb();
    case 0xDD: return exec_ddfd(&z.ix);
    case 0xFD: return exec_ddfd(&z.iy);
    case 0xED: return exec_ed();

    default:
        return 4; /* unknown opcode, treat as NOP */
    }
}

/* ------------------------------------------------------------------ */
/*  Init / Reset / Start                                               */
/* ------------------------------------------------------------------ */

void z80_init(void)
{
    memset(&z, 0, sizeof(z));
    memset(z80_ram, 0, Z80_RAM_SIZE);
}

void z80_reset(void)
{
    z.pc = 0;
    z.sp = 0;
    z.a = z.f = 0;
    z.b = z.c = z.d = z.e = z.h = z.l = 0;
    z.ix = z.iy = 0;
    z.iff1 = z.iff2 = 0;
    z.im = 0;
    z.halted = 0;
    z.running = 0;
}

void z80_release_reset(void)
{
    z.pc = 0;
    z.halted = 0;
    z.running = 1;
}

int z80_is_running(void)
{
    return z.running && !z.halted;
}

/* ------------------------------------------------------------------ */
/*  68K bus access to Z80 RAM                                          */
/* ------------------------------------------------------------------ */

uint8_t z80_ram_read(uint16_t addr)
{
    return z80_ram[addr & (Z80_RAM_SIZE - 1)];
}

void z80_ram_write(uint16_t addr, uint8_t val)
{
    z80_ram[addr & (Z80_RAM_SIZE - 1)] = val;
}

#include "cpu_internal.h"
#include "ea.h"
#include "memory.h"

/* 68K stack (A7) keeps word alignment: byte (An)+/-(An) use 2 for A7. */
int ea_step(int reg, int size)
{
    return (reg == 7 && size == 1) ? 2 : size;
}

uint32_t ea_fetch_value(int mode, int reg, int size)
{
    uint32_t addr, val;

    switch (mode) {
    case 0: /* Dn */
        val = cpu.d[reg];
        break;
    case 1: /* An */
        val = cpu.a[reg];
        break;
    case 2: /* (An) */
        addr = cpu.a[reg];
        if (size == 1) val = mem_read8(addr) & 0xFF;
        else if (size == 2) val = mem_read16(addr) & 0xFFFF;
        else val = mem_read32(addr);
        break;
    case 3: /* (An)+ */
        addr = cpu.a[reg];
        if (size == 1) val = mem_read8(addr) & 0xFF;
        else if (size == 2) val = mem_read16(addr) & 0xFFFF;
        else val = mem_read32(addr);
        cpu.a[reg] += ea_step(reg, size);
        break;
    case 4: /* -(An) */
        cpu.a[reg] -= ea_step(reg, size);
        addr = cpu.a[reg];
        if (size == 1) val = mem_read8(addr) & 0xFF;
        else if (size == 2) val = mem_read16(addr) & 0xFFFF;
        else val = mem_read32(addr);
        break;
    case 5: /* d(An) */
        addr = cpu.a[reg] + (int32_t)(int16_t)fetch16();
        if (size == 1) val = mem_read8(addr) & 0xFF;
        else if (size == 2) val = mem_read16(addr) & 0xFFFF;
        else val = mem_read32(addr);
        break;
    case 6: /* (d8,An,Xn) */
        {
            uint16_t ext = fetch16();
            int32_t disp = (int8_t)(ext >> 8);
            int idx_reg = ext & 0x0F;
            int idx_is_addr = (ext >> 6) & 1;
            int idx_long = (ext >> 4) & 1;
            uint32_t idx_val = idx_is_addr ? cpu.a[idx_reg] : cpu.d[idx_reg];
            if (!idx_long)
                idx_val = (uint32_t)(int32_t)(int16_t)(idx_val & 0xFFFF);
            addr = cpu.a[reg] + disp + idx_val;
        }
        if (size == 1) val = mem_read8(addr) & 0xFF;
        else if (size == 2) val = mem_read16(addr) & 0xFFFF;
        else val = mem_read32(addr);
        break;
    case 7:
        switch (reg) {
        case 0: /* abs.w */
            addr = (int32_t)(int16_t)fetch16();
            if (size == 1) val = mem_read8(addr) & 0xFF;
            else if (size == 2) val = mem_read16(addr) & 0xFFFF;
            else val = mem_read32(addr);
            break;
        case 1: /* abs.l */
            addr = fetch32();
            if (size == 1) val = mem_read8(addr) & 0xFF;
            else if (size == 2) val = mem_read16(addr) & 0xFFFF;
            else val = mem_read32(addr);
            break;
        case 2: /* d(PC) - PC is address of extension word before fetch */
            {
                uint32_t base = cpu.pc;
                int32_t disp = (int16_t)fetch16();
                addr = base + disp;
            }
            if (size == 1) val = mem_read8(addr) & 0xFF;
            else if (size == 2) val = mem_read16(addr) & 0xFFFF;
            else val = mem_read32(addr);
            break;
        case 3: /* (d8,PC,Xn) - PC is address of extension word before fetch */
            {
                uint32_t base = cpu.pc;
                uint16_t ext = fetch16();
                int32_t disp = (int8_t)(ext >> 8);
                int idx_reg = ext & 0x0F;
                int idx_is_addr = (ext >> 6) & 1;
                int idx_long = (ext >> 4) & 1;
                uint32_t idx_val = idx_is_addr ? cpu.a[idx_reg] : cpu.d[idx_reg];
                if (!idx_long)
                    idx_val = (uint32_t)(int32_t)(int16_t)(idx_val & 0xFFFF);
                addr = base + disp + idx_val;
            }
            if (size == 1) val = mem_read8(addr) & 0xFF;
            else if (size == 2) val = mem_read16(addr) & 0xFFFF;
            else val = mem_read32(addr);
            break;
        default:
            return 0;
        case 4: /* #imm */
            if (size == 1) val = fetch16() & 0xFF;
            else if (size == 2) val = fetch16() & 0xFFFF;
            else val = fetch32();
            break;
        }
        break;
    default:
        return 0;
    }

    return val;
}

void ea_store_value(int mode, int reg, int size, uint32_t value)
{
    uint32_t addr;

    switch (mode) {
    case 0: /* Dn */
        if (size == 1) cpu.d[reg] = (cpu.d[reg] & 0xFFFFFF00) | (value & 0xFF);
        else if (size == 2) cpu.d[reg] = (cpu.d[reg] & 0xFFFF0000) | (value & 0xFFFF);
        else cpu.d[reg] = value;
        break;
    case 1: /* An */
        if (size == 2) cpu.a[reg] = (cpu.a[reg] & 0xFFFF0000) | (value & 0xFFFF);
        else cpu.a[reg] = value;
        break;
    case 2: /* (An) */
        addr = cpu.a[reg];
        if (size == 1) mem_write8(addr, (uint8_t)(value & 0xFF));
        else if (size == 2) mem_write16(addr, (uint16_t)(value & 0xFFFF));
        else mem_write32(addr, value);
        break;
    case 3: /* (An)+ */
        addr = cpu.a[reg];
        if (size == 1) mem_write8(addr, (uint8_t)(value & 0xFF));
        else if (size == 2) mem_write16(addr, (uint16_t)(value & 0xFFFF));
        else mem_write32(addr, value);
        cpu.a[reg] += ea_step(reg, size);
        break;
    case 4: /* -(An) */
        cpu.a[reg] -= ea_step(reg, size);
        addr = cpu.a[reg];
        if (size == 1) mem_write8(addr, (uint8_t)(value & 0xFF));
        else if (size == 2) mem_write16(addr, (uint16_t)(value & 0xFFFF));
        else mem_write32(addr, value);
        break;
    case 5: /* d(An) */
        addr = cpu.a[reg] + (int32_t)(int16_t)fetch16();
        if (size == 1) mem_write8(addr, (uint8_t)(value & 0xFF));
        else if (size == 2) mem_write16(addr, (uint16_t)(value & 0xFFFF));
        else mem_write32(addr, value);
        break;
    case 6: /* (d8,An,Xn) */
        {
            uint16_t ext = fetch16();
            int32_t disp = (int8_t)(ext >> 8);
            int idx_reg = ext & 0x0F;
            int idx_is_addr = (ext >> 6) & 1;
            int idx_long = (ext >> 4) & 1;
            uint32_t idx_val = idx_is_addr ? cpu.a[idx_reg] : cpu.d[idx_reg];
            if (!idx_long)
                idx_val = (uint32_t)(int32_t)(int16_t)(idx_val & 0xFFFF);
            addr = cpu.a[reg] + disp + idx_val;
        }
        if (size == 1) mem_write8(addr, (uint8_t)(value & 0xFF));
        else if (size == 2) mem_write16(addr, (uint16_t)(value & 0xFFFF));
        else mem_write32(addr, value);
        break;
    case 7:
        switch (reg) {
        case 0: /* abs.w */
            addr = (int32_t)(int16_t)fetch16();
            if (size == 1) mem_write8(addr, (uint8_t)(value & 0xFF));
            else if (size == 2) mem_write16(addr, (uint16_t)(value & 0xFFFF));
            else mem_write32(addr, value);
            break;
        case 1: /* abs.l */
            addr = fetch32();
            if (size == 1) mem_write8(addr, (uint8_t)(value & 0xFF));
            else if (size == 2) mem_write16(addr, (uint16_t)(value & 0xFFFF));
            else mem_write32(addr, value);
            break;
        case 3: /* (d8,PC,Xn) */
            {
                uint32_t base = cpu.pc;
                uint16_t ext = fetch16();
                int32_t disp = (int8_t)(ext >> 8);
                int idx_reg = ext & 0x0F;
                int idx_is_addr = (ext >> 6) & 1;
                int idx_long = (ext >> 4) & 1;
                uint32_t idx_val = idx_is_addr ? cpu.a[idx_reg] : cpu.d[idx_reg];
                if (!idx_long)
                    idx_val = (uint32_t)(int32_t)(int16_t)(idx_val & 0xFFFF);
                addr = base + disp + idx_val;
            }
            if (size == 1) mem_write8(addr, (uint8_t)(value & 0xFF));
            else if (size == 2) mem_write16(addr, (uint16_t)(value & 0xFFFF));
            else mem_write32(addr, value);
            break;
        default:
            break;
        }
        break;
    default:
        break;
    }
}

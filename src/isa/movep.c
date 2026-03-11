/*
 * MOVEP: Move Peripheral. Transfers between Dn and alternate bytes in memory.
 * Used for 8-bit peripherals at odd addresses (e.g. Amiga custom chips).
 * EA: d(An) only. 16-bit displacement follows. Word (2 bytes) or Long (4 bytes).
 * Order: high byte first. Condition codes not affected.
 * Encoding: 0x0108-0x010F (store W), 0x0148-0x014F (store L),
 *          0x0188-0x018F (load W), 0x01C8-0x01CF (load L).
 */

#include "cpu_internal.h"
#include "memory.h"
#include "timing.h"

/* MOVEP: (op & 0xF1F8) in {0x0108, 0x0148, 0x0188, 0x01C8}. */
static int is_movep(uint16_t op)
{
    uint16_t base = op & 0xF1F8;
    return base == 0x0108 || base == 0x0148 || base == 0x0188 || base == 0x01C8;
}

int op_movep(uint16_t op)
{
    if (!is_movep(op))
        return 0;

    int dn = (op >> 9) & 7;
    int an = op & 7;
    int is_load = (op & 0x0100) != 0;   /* bit 8: 0=store (0x0108/0x0148), 1=load (0x0188/0x01C8) */
    int is_long = (op & 0x0040) != 0;   /* bit 6: 0=word, 1=long */
    int size = is_long ? 4 : 2;

    int32_t disp = (int32_t)(int16_t)fetch16();
    uint32_t addr = cpu.a[an] + disp;

    uint32_t val = is_load ? 0 : cpu.d[dn];
    for (int i = 0; i < size; i++) {
        int shift = (size - 1 - i) * 8;
        if (is_load)
            val |= (uint32_t)mem_read8(addr + i * 2) << shift;
        else
            mem_write8(addr + i * 2, (uint8_t)(val >> shift));
    }
    if (is_load) {
        if (size == 2)
            val = (uint32_t)(int32_t)(int16_t)(val & 0xFFFF);  /* sign-extend word */
        cpu.d[dn] = val;
    }
    return movep_cycles(size);
}

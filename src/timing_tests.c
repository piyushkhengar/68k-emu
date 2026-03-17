/*
 * Unit tests for cycle timing functions.
 * Validates move_cycles, ea_cycles, divu_cycles, divs_cycles, shift_cycles,
 * and add_sub_cycles against Motorola MC68000 reference values.
 */

#include "timing.h"
#include <stdio.h>

static int timing_failures;
static int timing_total;

#define TASSERT(expr, fmt, ...) do { \
    timing_total++; \
    if (!(expr)) { \
        timing_failures++; \
        printf("    FAIL: " fmt "\n", ##__VA_ARGS__); \
    } \
} while (0)

#define TCHECK(fn_call, expected, label) do { \
    int _got = (fn_call); \
    timing_total++; \
    if (_got != (expected)) { \
        timing_failures++; \
        printf("    FAIL: %s: expected %d, got %d\n", (label), (expected), _got); \
    } \
} while (0)

/* ea_cycles: verify against Motorola Table 8-1 */
static void test_ea_cycles(void)
{
    /* Dn, An: always 0 */
    TCHECK(ea_cycles(0, 0, 1), 0, "ea Dn byte");
    TCHECK(ea_cycles(1, 0, 4), 0, "ea An long");

    /* (An): 4 B/W, 8 L */
    TCHECK(ea_cycles(2, 0, 1), 4, "ea (An) byte");
    TCHECK(ea_cycles(2, 0, 4), 8, "ea (An) long");

    /* (An)+: same as (An) */
    TCHECK(ea_cycles(3, 0, 2), 4, "ea (An)+ word");
    TCHECK(ea_cycles(3, 0, 4), 8, "ea (An)+ long");

    /* -(An): 6 B/W, 10 L */
    TCHECK(ea_cycles(4, 0, 1), 6, "ea -(An) byte");
    TCHECK(ea_cycles(4, 0, 4), 10, "ea -(An) long");

    /* d(An): 8 B/W, 12 L */
    TCHECK(ea_cycles(5, 0, 2), 8, "ea d(An) word");
    TCHECK(ea_cycles(5, 0, 4), 12, "ea d(An) long");

    /* d(An,Xi): 10 B/W, 14 L */
    TCHECK(ea_cycles(6, 0, 2), 10, "ea d(An,Xi) word");
    TCHECK(ea_cycles(6, 0, 4), 14, "ea d(An,Xi) long");

    /* abs.w: 8 B/W, 12 L */
    TCHECK(ea_cycles(7, 0, 1), 8, "ea abs.w byte");
    TCHECK(ea_cycles(7, 0, 4), 12, "ea abs.w long");

    /* abs.l: 12 B/W, 16 L */
    TCHECK(ea_cycles(7, 1, 2), 12, "ea abs.l word");
    TCHECK(ea_cycles(7, 1, 4), 16, "ea abs.l long");

    /* d(PC): 8 B/W, 12 L */
    TCHECK(ea_cycles(7, 2, 2), 8, "ea d(PC) word");
    TCHECK(ea_cycles(7, 2, 4), 12, "ea d(PC) long");

    /* d(PC,Xi): 10 B/W, 14 L */
    TCHECK(ea_cycles(7, 3, 1), 10, "ea d(PC,Xi) byte");
    TCHECK(ea_cycles(7, 3, 4), 14, "ea d(PC,Xi) long");

    /* #imm: 4 B/W, 8 L */
    TCHECK(ea_cycles(7, 4, 2), 4, "ea #imm word");
    TCHECK(ea_cycles(7, 4, 4), 8, "ea #imm long");
}

/*
 * move_cycles: test asymmetric source/dest pairs.
 * The table is [source][dest], so move(src,dst) != move(dst,src) when
 * source and dest have different base timings.
 * Reference: Motorola MC68000 User's Manual Table 8-4.
 */
static void test_move_cycles(void)
{
    /* Symmetric: Dn↔Dn always 4 */
    TCHECK(move_cycles(0, 0, 0, 0, 2), 4, "MOVE.W Dn,Dn");
    TCHECK(move_cycles(0, 0, 0, 0, 4), 4, "MOVE.L Dn,Dn");

    /* Asymmetric B/W: -(An) vs Dn */
    TCHECK(move_cycles(4, 0, 0, 0, 2), 10, "MOVE.W -(An),Dn");
    TCHECK(move_cycles(0, 0, 4, 0, 2), 8,  "MOVE.W Dn,-(An)");

    /* Asymmetric B/W: d(An) vs Dn */
    TCHECK(move_cycles(5, 0, 0, 0, 2), 12, "MOVE.W d(An),Dn");
    TCHECK(move_cycles(0, 0, 5, 0, 2), 12, "MOVE.W Dn,d(An)");

    /* Asymmetric B/W: abs.l vs Dn */
    TCHECK(move_cycles(7, 1, 0, 0, 2), 16, "MOVE.W abs.l,Dn");
    TCHECK(move_cycles(0, 0, 7, 1, 2), 16, "MOVE.W Dn,abs.l");

    /* Asymmetric B/W: -(An) vs (An) */
    TCHECK(move_cycles(4, 0, 2, 0, 2), 14, "MOVE.W -(An),(An)");
    TCHECK(move_cycles(2, 0, 4, 0, 2), 12, "MOVE.W (An),-(An)");

    /* Asymmetric B/W: #imm to d(An) */
    TCHECK(move_cycles(7, 4, 5, 0, 2), 16, "MOVE.W #imm,d(An)");

    /* Asymmetric L: -(An) vs Dn */
    TCHECK(move_cycles(4, 0, 0, 0, 4), 14, "MOVE.L -(An),Dn");
    TCHECK(move_cycles(0, 0, 4, 0, 4), 12, "MOVE.L Dn,-(An)");

    /* L: #imm to d(An) -- table row 11, col 5 = 24 */
    TCHECK(move_cycles(7, 4, 5, 0, 4), 24, "MOVE.L #imm,d(An)");

    /* L: (An) to/from abs.l -- both 28 per Motorola table */
    TCHECK(move_cycles(2, 0, 7, 1, 4), 28, "MOVE.L (An),abs.l");
    TCHECK(move_cycles(7, 1, 2, 0, 4), 28, "MOVE.L abs.l,(An)");

    /* Byte: -(An) vs (An)+ */
    TCHECK(move_cycles(4, 0, 3, 0, 1), 14, "MOVE.B -(An),(An)+");
    TCHECK(move_cycles(3, 0, 4, 0, 1), 12, "MOVE.B (An)+,-(An)");
}

/* add_sub_cycles: verify direction and size-dependent base */
static void test_add_sub_cycles(void)
{
    /* <ea> to Dn (dir=0): base 4 B/W, 6 L (or 8 L if Dn/An/#imm) */
    TCHECK(add_sub_cycles(0, 0, 1, 0), 4, "ADD.B Dn,Dn (dir=0)");
    TCHECK(add_sub_cycles(0, 0, 4, 0), 8, "ADD.L Dn,Dn (dir=0, base 8)");
    TCHECK(add_sub_cycles(2, 0, 4, 0), 14, "ADD.L (An),Dn (dir=0, base 6+8)");
    TCHECK(add_sub_cycles(7, 4, 4, 0), 16, "ADD.L #imm,Dn (dir=0, base 8+8)");

    /* Dn to <ea> (dir=1): base 8 B/W, 12 L */
    TCHECK(add_sub_cycles(2, 0, 2, 1), 12, "ADD.W Dn,(An) (dir=1, 8+4)");
    TCHECK(add_sub_cycles(2, 0, 4, 1), 20, "ADD.L Dn,(An) (dir=1, 12+8)");
}

/* shift_cycles_register: base 6/8 + 2*count */
static void test_shift_cycles(void)
{
    TCHECK(shift_cycles_register(1, 0, 0), 6, "shift byte count=0");
    TCHECK(shift_cycles_register(2, 1, 0), 8, "shift word count=1");
    TCHECK(shift_cycles_register(2, 4, 0), 14, "shift word count=4");
    TCHECK(shift_cycles_register(4, 0, 0), 8, "shift long count=0");
    TCHECK(shift_cycles_register(4, 3, 0), 14, "shift long count=3");
    TCHECK(shift_cycles_register(4, 8, 0), 24, "shift long count=8");

    /* Memory shifts: 8 + ea_cycles(word) */
    TCHECK(shift_cycles_memory(2, 0), 12, "shift mem (An)");
    TCHECK(shift_cycles_memory(5, 0), 16, "shift mem d(An)");
}

/* divu_cycles: test against ijor's reference values */
static void test_divu_cycles(void)
{
    /* Div by zero → 0 (exception handles timing) */
    TCHECK(divu_cycles(0, 0, 100, 0), 0, "DIVU div-by-zero");

    /* Overflow: (dividend >> 16) >= divisor → 10 */
    TCHECK(divu_cycles(0, 0, 0x00020000, 1), 10, "DIVU overflow (0x20000/1)");
    TCHECK(divu_cycles(0, 0, 0x00010000, 1), 10, "DIVU overflow (0x10000/1)");
    TCHECK(divu_cycles(0, 0, 0xFFFF0000, 0xFFFF), 10, "DIVU overflow (0xFFFF0000/0xFFFF)");

    /* Worst case: quotient all zeros → 136 register cycles */
    TCHECK(divu_cycles(0, 0, 0, 1), 136, "DIVU worst case (0/1)");

    /* Range check: all results should be 76-136 for non-overflow register */
    int c1 = divu_cycles(0, 0, 100, 10);
    TASSERT(c1 >= 76 && c1 <= 136, "DIVU 100/10 in range: got %d", c1);

    int c2 = divu_cycles(0, 0, 0xFFFE, 1);
    TASSERT(c2 >= 76 && c2 <= 136, "DIVU 0xFFFE/1 in range: got %d", c2);

    int c3 = divu_cycles(0, 0, 0x0001FFFE, 2);
    TASSERT(c3 >= 76 && c3 <= 136, "DIVU 0x1FFFE/2 in range: got %d", c3);

    /* With EA: adds ea_cycles on top */
    TCHECK(divu_cycles(0, 0, 0x00020000, 1) + 0, 10, "DIVU overflow reg");
    int c4 = divu_cycles(2, 0, 0, 1);
    TCHECK(c4, 136 + 4, "DIVU worst case (An) = 136+4");
}

/* divs_cycles: test against ijor's reference values */
static void test_divs_cycles(void)
{
    /* Div by zero → 0 */
    TCHECK(divs_cycles(0, 0, 100, 0), 0, "DIVS div-by-zero");

    /* Overflow: positive dividend → (6+2)*2 = 16 */
    TCHECK(divs_cycles(0, 0, 0x7FFFFFFF, 1), 16, "DIVS overflow pos dividend");

    /* Overflow: negative dividend → (7+2)*2 = 18 */
    TCHECK(divs_cycles(0, 0, (int32_t)0x80000000, 1), 18, "DIVS overflow neg dividend");

    /* Range check: non-overflow results in 120-156 */
    int c1 = divs_cycles(0, 0, 100, 10);
    TASSERT(c1 >= 120 && c1 <= 156, "DIVS 100/10 in range: got %d", c1);

    int c2 = divs_cycles(0, 0, -100, 10);
    TASSERT(c2 >= 120 && c2 <= 156, "DIVS -100/10 in range: got %d", c2);

    int c3 = divs_cycles(0, 0, 100, -10);
    TASSERT(c3 >= 120 && c3 <= 156, "DIVS 100/-10 in range: got %d", c3);

    int c4 = divs_cycles(0, 0, -100, -10);
    TASSERT(c4 >= 120 && c4 <= 156, "DIVS -100/-10 in range: got %d", c4);

    /* With EA: adds ea_cycles */
    int c5 = divs_cycles(2, 0, 100, 10);
    TASSERT(c5 == c1 + 4, "DIVS (An) ea adds 4: got %d, expected %d", c5, c1 + 4);
}

int run_timing_tests(void)
{
    timing_failures = 0;
    timing_total = 0;

    printf("  Timing unit tests:\n");
    test_ea_cycles();
    test_move_cycles();
    test_add_sub_cycles();
    test_shift_cycles();
    test_divu_cycles();
    test_divs_cycles();

    printf("    %d/%d timing assertions passed\n", timing_total - timing_failures, timing_total);
    return timing_failures;
}

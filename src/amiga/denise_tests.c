/*
 * Unit tests for src/amiga/denise.c — Chapter 3 step 2.
 *
 * Tests are written directly against the denise_t struct and render
 * into local pixel buffers — no bus routing, no CPU execution, no SDL2.
 *
 * Coverage:
 *   - Init state (colours and pointers all zero)
 *   - Colour register write and read-back
 *   - denise_expand_color: 12-bit → 32-bit ARGB for black, white, primaries, mid
 *   - denise_render_line with 0 planes (all background colour)
 *   - denise_render_line with 1 plane, all-ones data (all foreground)
 *   - denise_render_line with 1 plane, alternating pattern (check bit order)
 *   - denise_render_line with 2 planes (4-colour index assembly)
 *   - denise_render_line with 4 planes (16-colour, all planes set)
 *   - Bitplane pointer write: high word sets bits 20:16, low word sets 15:1
 *   - Unknown register read returns 0; unknown write silently ignored
 */

#include "denise.h"
#include "denise_tests.h"
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Test framework                                                      */
/* ------------------------------------------------------------------ */

static int failures;
static int total;

#define BASSERT(expr, fmt, ...) do {                                    \
    total++;                                                            \
    if (!(expr)) {                                                      \
        failures++;                                                     \
        printf("    FAIL [%s:%d]: " fmt "\n", __func__, __LINE__,      \
               ##__VA_ARGS__);                                          \
    }                                                                   \
} while (0)

/* ------------------------------------------------------------------ */
/*  Helpers                                                             */
/* ------------------------------------------------------------------ */

static denise_t d;

static void reset_denise(void)
{
    denise_init(&d);
}

/*
 * Small chip RAM used by render tests.
 * 4 KB is more than enough: 320 pixels × 6 planes × 2 bytes/word = 480 bytes.
 */
#define TEST_RAM_SIZE 4096u
static uint8_t test_ram[TEST_RAM_SIZE];

/* Fill a bitplane region with a repeating 16-bit word pattern. */
static void fill_plane(uint32_t base, uint16_t pattern, int num_words)
{
    for (int i = 0; i < num_words; i++) {
        test_ram[base + i * 2 + 0] = (uint8_t)(pattern >> 8);
        test_ram[base + i * 2 + 1] = (uint8_t)(pattern & 0xFF);
    }
}

/* ------------------------------------------------------------------ */
/*  Test: init state                                                    */
/* ------------------------------------------------------------------ */

static void test_init_state(void)
{
    reset_denise();
    for (int i = 0; i < DENISE_COLORS; i++)
        BASSERT(d.color[i] == 0,
                "color[%d] should be 0 after init", i);
    BASSERT(d.bplcon0 == 0, "bplcon0 should be 0 after init");
    for (int n = 0; n < DENISE_PLANES; n++)
        BASSERT(d.bplpt[n] == 0,
                "bplpt[%d] should be 0 after init", n);
}

/* ------------------------------------------------------------------ */
/*  Test: colour register write and read-back                          */
/* ------------------------------------------------------------------ */

static void test_color_write_read(void)
{
    reset_denise();

    denise_write_reg(&d, 0x180, 0x0F00);   /* COLOR00 = red */
    BASSERT(d.color[0] == 0x0F00,
            "color[0] should be 0x0F00 after write");
    BASSERT(denise_read_reg(&d, 0x180) == 0x0F00,
            "read_reg(0x180) should return 0x0F00");

    denise_write_reg(&d, 0x1BE, 0x0FFF);   /* COLOR31 = white */
    BASSERT(d.color[31] == 0x0FFF,
            "color[31] should be 0x0FFF after write");

    /* Upper nibble must be masked off */
    denise_write_reg(&d, 0x182, 0xF123);   /* COLOR01 — upper nibble F */
    BASSERT(d.color[1] == 0x0123,
            "color[1] upper nibble should be masked to 0");
}

/* ------------------------------------------------------------------ */
/*  Test: 12-bit → 32-bit colour expansion                             */
/* ------------------------------------------------------------------ */

static void test_expand_black(void)
{
    BASSERT(denise_expand_color(0x0000) == 0xFF000000u,
            "black: expected 0xFF000000, got 0x%08X",
            denise_expand_color(0x0000));
}

static void test_expand_white(void)
{
    BASSERT(denise_expand_color(0x0FFF) == 0xFFFFFFFFu,
            "white: expected 0xFFFFFFFF, got 0x%08X",
            denise_expand_color(0x0FFF));
}

static void test_expand_red(void)
{
    /* 0x0F00: R=F, G=0, B=0 → 0xFFFF0000 */
    BASSERT(denise_expand_color(0x0F00) == 0xFFFF0000u,
            "red: expected 0xFFFF0000, got 0x%08X",
            denise_expand_color(0x0F00));
}

static void test_expand_green(void)
{
    /* 0x00F0: R=0, G=F, B=0 → 0xFF00FF00 */
    BASSERT(denise_expand_color(0x00F0) == 0xFF00FF00u,
            "green: expected 0xFF00FF00, got 0x%08X",
            denise_expand_color(0x00F0));
}

static void test_expand_blue(void)
{
    /* 0x000F: R=0, G=0, B=F → 0xFF0000FF */
    BASSERT(denise_expand_color(0x000F) == 0xFF0000FFu,
            "blue: expected 0xFF0000FF, got 0x%08X",
            denise_expand_color(0x000F));
}

static void test_expand_mid_nibbles(void)
{
    /*
     * 0x0123: R=1, G=2, B=3
     * Nibble replication: 1→0x11, 2→0x22, 3→0x33
     * Expected: 0xFF112233
     */
    BASSERT(denise_expand_color(0x0123) == 0xFF112233u,
            "mid: expected 0xFF112233, got 0x%08X",
            denise_expand_color(0x0123));
}

static void test_expand_amiga_grey(void)
{
    /*
     * The Kickstart 1.3 boot screen uses colour 0x0AAA as the grey background.
     * Nibble A = 1010 → 0xAA.  Expected: 0xFFAAAAAA.
     */
    BASSERT(denise_expand_color(0x0AAA) == 0xFFAAAAAAu,
            "kickstart grey: expected 0xFFAAAAAA, got 0x%08X",
            denise_expand_color(0x0AAA));
}

/* ------------------------------------------------------------------ */
/*  Test: render with 0 planes — all pixels show background colour     */
/* ------------------------------------------------------------------ */

static void test_render_0_planes(void)
{
    reset_denise();
    memset(test_ram, 0, TEST_RAM_SIZE);

    d.bplcon0 = 0x0000;                      /* BPU = 0 */
    d.color[0] = 0x0AAA;                     /* background = grey */

    uint32_t pixels[DENISE_HIRES_W];
    denise_render_line(&d, test_ram, TEST_RAM_SIZE, pixels, 0, 0, DENISE_HIRES_W);

    uint32_t expected = denise_expand_color(0x0AAA);
    for (int px = 0; px < DENISE_HIRES_W; px++) {
        BASSERT(pixels[px] == expected,
                "px %d: expected 0x%08X (background), got 0x%08X",
                px, expected, pixels[px]);
        if (pixels[px] != expected) break;   /* stop spam on first mismatch */
    }
}

/* ------------------------------------------------------------------ */
/*  Test: 1 plane, all 1s — every pixel is colour[1]                  */
/* ------------------------------------------------------------------ */

static void test_render_1_plane_all_set(void)
{
    reset_denise();
    memset(test_ram, 0, TEST_RAM_SIZE);

    d.bplcon0   = 0x1000;     /* BPU = 1 */
    d.bplpt[0]  = 0x0100;     /* plane 0 at 0x0100 in test_ram */
    d.color[0]  = 0x0000;     /* background = black */
    d.color[1]  = 0x0F00;     /* foreground = red */

    /* Fill plane 0 with 0xFFFF (all bits set → all pixels colour[1]) */
    fill_plane(0x0100, 0xFFFF, DENISE_LORES_W / 16);

    uint32_t pixels[DENISE_HIRES_W];
    denise_render_line(&d, test_ram, TEST_RAM_SIZE, pixels, 0, 0, DENISE_HIRES_W);

    uint32_t expected = denise_expand_color(0x0F00);
    for (int px = 0; px < DENISE_HIRES_W; px++) {
        BASSERT(pixels[px] == expected,
                "px %d: expected 0x%08X (red), got 0x%08X",
                px, expected, pixels[px]);
        if (pixels[px] != expected) break;
    }
}

/* ------------------------------------------------------------------ */
/*  Test: 1 plane, alternating 0xAAAA — checks bit order              */
/* ------------------------------------------------------------------ */

static void test_render_1_plane_alternating(void)
{
    reset_denise();
    memset(test_ram, 0, TEST_RAM_SIZE);

    d.bplcon0   = 0x1000;    /* BPU = 1 */
    d.bplpt[0]  = 0x0200;
    d.color[0]  = 0x0000;   /* background = black */
    d.color[1]  = 0x00F0;   /* foreground = green */

    /*
     * 0xAAAA = 1010 1010 1010 1010
     *          ^--- bit 15 = MSB = pixel 0 = 1 (foreground)
     *           ^-- bit 14 = pixel 1 = 0 (background)
     * So pixels 0, 2, 4, … = colour[1]; pixels 1, 3, 5, … = colour[0].
     */
    fill_plane(0x0200, 0xAAAA, DENISE_LORES_W / 16);

    uint32_t pixels[DENISE_HIRES_W];
    denise_render_line(&d, test_ram, TEST_RAM_SIZE, pixels, 0, 0, DENISE_HIRES_W);

    uint32_t fg = denise_expand_color(0x00F0);
    uint32_t bg = denise_expand_color(0x0000);

    /*
     * LORES with doubled output: source pixel N → output pixels N*2, N*2+1.
     * 0xAAAA: source pixels 0,2,4… = fg; 1,3,5… = bg.
     * Output: fg,fg,bg,bg,fg,fg,bg,bg,…
     */
    for (int px = 0; px < DENISE_HIRES_W; px++) {
        uint32_t expected = ((px / 2) % 2 == 0) ? fg : bg;
        BASSERT(pixels[px] == expected,
                "px %d: expected 0x%08X (%s), got 0x%08X",
                px, expected, ((px / 2) % 2 == 0) ? "fg" : "bg", pixels[px]);
        if (pixels[px] != expected) break;
    }
}

/* ------------------------------------------------------------------ */
/*  Test: 2 planes — 4-colour index assembly                          */
/* ------------------------------------------------------------------ */

static void test_render_2_planes(void)
{
    reset_denise();
    memset(test_ram, 0, TEST_RAM_SIZE);

    d.bplcon0  = 0x2000;   /* BPU = 2 */
    d.bplpt[0] = 0x0300;   /* plane 0 base */
    d.bplpt[1] = 0x0340;   /* plane 1 base (320/16=20 words × 2 bytes = 40 bytes apart) */

    d.color[0] = 0x0000;   /* index 00 = black */
    d.color[1] = 0x0F00;   /* index 01 = red   (plane 0 set, plane 1 clear) */
    d.color[2] = 0x00F0;   /* index 10 = green (plane 0 clear, plane 1 set) */
    d.color[3] = 0x0FFF;   /* index 11 = white (both planes set) */

    /* Plane 0: all 1s → bit 0 of index always 1 */
    fill_plane(0x0300, 0xFFFF, DENISE_LORES_W / 16);
    /* Plane 1: all 0s → bit 1 of index always 0 */
    fill_plane(0x0340, 0x0000, DENISE_LORES_W / 16);
    /* Expected colour index everywhere = 0b01 = 1 = red */

    uint32_t pixels[DENISE_HIRES_W];
    denise_render_line(&d, test_ram, TEST_RAM_SIZE, pixels, 0, 0, DENISE_HIRES_W);

    uint32_t expected = denise_expand_color(0x0F00);
    for (int px = 0; px < DENISE_HIRES_W; px++) {
        BASSERT(pixels[px] == expected,
                "px %d: 2-plane index 01 should give red (0x%08X), got 0x%08X",
                px, expected, pixels[px]);
        if (pixels[px] != expected) break;
    }
}

/* ------------------------------------------------------------------ */
/*  Test: 4 planes, all set — index = 0b1111 = 15                     */
/* ------------------------------------------------------------------ */

static void test_render_4_planes_all_set(void)
{
    reset_denise();
    memset(test_ram, 0, TEST_RAM_SIZE);

    d.bplcon0 = 0x4000;   /* BPU = 4 */
    for (int n = 0; n < 4; n++) {
        d.bplpt[n] = (uint32_t)(0x0400 + n * 0x40);  /* 64 bytes apart */
        fill_plane(d.bplpt[n], 0xFFFF, DENISE_LORES_W / 16);
    }
    d.color[15] = 0x0F8F;   /* index 1111 → this colour */

    uint32_t pixels[DENISE_HIRES_W];
    denise_render_line(&d, test_ram, TEST_RAM_SIZE, pixels, 0, 0, DENISE_HIRES_W);

    uint32_t expected = denise_expand_color(0x0F8F);
    for (int px = 0; px < DENISE_HIRES_W; px++) {
        BASSERT(pixels[px] == expected,
                "px %d: 4-plane all-set should give color[15] (0x%08X), got 0x%08X",
                px, expected, pixels[px]);
        if (pixels[px] != expected) break;
    }
}

/* ------------------------------------------------------------------ */
/*  Test: bitplane pointer high/low word assembly                      */
/* ------------------------------------------------------------------ */

static void test_bplpt_high_low(void)
{
    reset_denise();

    /* Write BPL1PTH (0x0E0) then BPL1PTL (0x0E2) */
    denise_write_reg(&d, 0x0E0, 0x0003);   /* high: bits 20:16 = 0x03 */
    denise_write_reg(&d, 0x0E2, 0xABCE);   /* low:  bottom bit cleared to 0 */

    /*
     * Expected: (0x03 << 16) | (0xABCE & 0xFFFE)
     *         = 0x030000 | 0xABCE
     *         = 0x03ABCE
     */
    BASSERT(d.bplpt[0] == 0x03ABCEu,
            "bplpt[0] should be 0x03ABCE, got 0x%06X", d.bplpt[0]);

    /* Write BPL3PTH/L (0x0E8/0x0EA) for plane index 2 */
    denise_write_reg(&d, 0x0E8, 0x0001);   /* high: bit 16 set */
    denise_write_reg(&d, 0x0EA, 0x0000);   /* low: 0 */
    BASSERT(d.bplpt[2] == 0x010000u,
            "bplpt[2] should be 0x010000, got 0x%06X", d.bplpt[2]);
}

/* ------------------------------------------------------------------ */
/*  Test: unknown register returns 0; write silently ignored           */
/* ------------------------------------------------------------------ */

static void test_unknown_register(void)
{
    reset_denise();
    d.color[0] = 0x0ABC;

    BASSERT(denise_read_reg(&d, 0x100) == 0,
            "BPLCON0 should return 0 (write-only)");

    denise_write_reg(&d, 0x0FE, 0xDEAD);   /* unknown offset */
    BASSERT(d.color[0] == 0x0ABC,
            "unknown write should not corrupt color[0]");
}

/* ------------------------------------------------------------------ */
/*  Public entry point                                                  */
/* ------------------------------------------------------------------ */

int run_denise_tests(void)
{
    failures = 0;
    total    = 0;

    printf("  [denise] colour registers + bitplane pipeline\n");

    test_init_state();
    test_color_write_read();
    test_expand_black();
    test_expand_white();
    test_expand_red();
    test_expand_green();
    test_expand_blue();
    test_expand_mid_nibbles();
    test_expand_amiga_grey();
    test_render_0_planes();
    test_render_1_plane_all_set();
    test_render_1_plane_alternating();
    test_render_2_planes();
    test_render_4_planes_all_set();
    test_bplpt_high_low();
    test_unknown_register();

    printf("  [denise] %d/%d passed\n", total - failures, total);
    return failures > 0 ? 1 : 0;
}

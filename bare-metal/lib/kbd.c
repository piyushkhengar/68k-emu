/* bare-metal/lib/kbd.c
 * PS/2 keyboard driver via I/O ports 0x60 (data) / 0x64 (status).
 * Scancode set 1, US QWERTY layout.
 * Also tracks live key state for joypad polling (kbd_joypad_state).
 */

#include "bare.h"
#include "io.h"

#define KBD_DATA 0x60
#define KBD_STAT 0x64

static inline uint8_t inb(uint16_t port)
{
    uint8_t val;
    __asm__ volatile ("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

/* US QWERTY scancode set 1 → ASCII (0 = non-printable / modifier) */
static const char sc_ascii[128] = {
    0,    27,   '1',  '2',  '3',  '4',  '5',  '6',  /* 00-07 */
    '7',  '8',  '9',  '0',  '-',  '=',  '\b', '\t', /* 08-0F */
    'q',  'w',  'e',  'r',  't',  'y',  'u',  'i',  /* 10-17 */
    'o',  'p',  '[',  ']',  '\n', 0,    'a',  's',  /* 18-1F */
    'd',  'f',  'g',  'h',  'j',  'k',  'l',  ';',  /* 20-27 */
    '\'', '`',  0,    '\\', 'z',  'x',  'c',  'v',  /* 28-2F */
    'b',  'n',  'm',  ',',  '.',  '/',  0,    '*',  /* 30-37 */
    0,    ' ',  0,    0,    0,    0,    0,    0,    /* 38-3F */
    0,    0,    0,    0,    0,    0,    0,    '7',  /* 40-47 */
    '8',  '9',  '-',  '4',  '5',  '6',  '+',  '1',  /* 48-4F */
    '2',  '3',  '0',  '.',  0,    0,    0,    0,    /* 50-57 */
    0,    0,    0,    0,    0,    0,    0,    0,    /* 58-5F */
    0,    0,    0,    0,    0,    0,    0,    0,    /* 60-67 */
    0,    0,    0,    0,    0,    0,    0,    0,    /* 68-6F */
    0,    0,    0,    0,    0,    0,    0,    0,    /* 70-77 */
    0,    0,    0,    0,    0,    0,    0,    0,    /* 78-7F */
};

/* ---- Joypad key state ---------------------------------------------------- */
/*
 * Keyboard → Genesis controller mapping:
 *   Arrow keys    → D-pad       (extended scancodes: E0 48/50/4B/4D)
 *   Z             → A button    (scancode 0x2C)
 *   X             → B button    (scancode 0x2D)
 *   C             → C button    (scancode 0x2E)
 *   Enter         → Start       (scancode 0x1C)
 *   Escape        → quit flag
 */

static uint8_t joypad_state = 0;
static int     quit_pressed = 0;
static int     ext_pending  = 0;   /* 1 if we just saw 0xE0 */

static void process_scancode(uint8_t sc)
{
    if (sc == 0xE0) { ext_pending = 1; return; }

    int ext     = ext_pending;
    int release = (sc & 0x80) != 0;
    ext_pending = 0;
    uint8_t key = sc & 0x7F;

    uint8_t btn = 0;

    if (ext) {
        /* Extended keys (arrow keys) */
        if      (key == 0x48) btn = PAD_UP;
        else if (key == 0x50) btn = PAD_DOWN;
        else if (key == 0x4B) btn = PAD_LEFT;
        else if (key == 0x4D) btn = PAD_RIGHT;
    } else {
        /* Normal keys */
        if      (key == 0x2C) btn = PAD_A;        /* Z */
        else if (key == 0x2D) btn = PAD_B;        /* X */
        else if (key == 0x2E) btn = PAD_C;        /* C */
        else if (key == 0x1C) btn = PAD_START;    /* Enter */
        else if (key == 0x01 && !release) quit_pressed = 1; /* Escape */
    }

    if (btn) {
        if (release) joypad_state &= ~btn;
        else         joypad_state |=  btn;
    }
}

/* Call every frame (or more often) to drain the keyboard buffer and update
 * the joypad state. Also pushes the state into io.c. */
void kbd_poll_joypad(void)
{
    while (inb(KBD_STAT) & 1)
        process_scancode(inb(KBD_DATA));

    io_set_pad(0, joypad_state);
}

int kbd_quit_pressed(void)
{
    return quit_pressed;
}

/* ---- Text input API (used by interactive stepper) ----------------------- */

int kbd_ready(void)
{
    return (inb(KBD_STAT) & 1) != 0;
}

/* Returns ASCII character, or 0 for non-printable keys / key releases.
 * Also updates joypad state as a side effect. */
char kbd_getchar(void)
{
    uint8_t sc = inb(KBD_DATA);
    process_scancode(sc);
    io_set_pad(0, joypad_state);
    if (sc & 0x80) return 0;
    if (sc >= 128)  return 0;
    return sc_ascii[sc & 0x7F];
}

/* Blocking: spin until a printable key is pressed */
char kbd_wait(void)
{
    for (;;) {
        while (!kbd_ready()) {}
        char c = kbd_getchar();
        if (c) return c;
    }
}

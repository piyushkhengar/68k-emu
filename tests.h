/*
 * Built-in test ROMs and test table for 68k-emu.
 */

#ifndef TESTS_H
#define TESTS_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char *name;
    const uint8_t *rom;
    size_t size;
    const char *description;
    int max_steps;  /* 0 = default 100 */
} builtin_test_t;

/* Default NOP loop when no ROM specified */
extern const uint8_t nop_loop[];
extern const size_t nop_loop_size;

/* Look up built-in test by name. Returns NULL if not found. */
const builtin_test_t *find_builtin_test(const char *name);

#endif /* TESTS_H */

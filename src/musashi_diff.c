/*
 * musashi_diff.c — Differential validation: our 68k emulator vs Musashi
 *
 * For each test in a SingleStepTests/680x0 JSON directory, both emulators
 * are given identical initial state.  After execution their outputs are
 * compared.  Divergences between our emulator and Musashi are printed so
 * they can be investigated as real bugs.
 *
 * Three outcome categories per test:
 *   PASS          — both match the corpus expected state
 *   CORPUS_DIFF   — we agree with Musashi but the corpus expected differs
 *                   (likely a test-suite convention, not a bug in either emu)
 *   REAL_DIFF     — we disagree with Musashi (printed by default)
 *
 * Build:  make musashi-diff
 * Run:    ./68k-musashi-diff ProcessorTests/68000/v1 [FILTER]
 *         ./68k-musashi-diff ProcessorTests/68000/v1 ADD   # only ADD* files
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <stdint.h>

/* Our emulator */
#include "cpu.h"
#include "cpu_internal.h"
#include "memory.h"

/* Musashi */
#include "m68k.h"
#include "m68kcpu.h"   /* for m68ki_cpu direct-field access */

/* cJSON */
#include "deps/cJSON/cJSON.h"

#ifdef HAVE_ZLIB
#include <zlib.h>
#endif

/* ======================================================================== */
/* Musashi memory backend — separate 16 MB flat buffer                       */
/* ======================================================================== */

#define MUSASHI_MEM_SIZE (16 * 1024 * 1024)
static uint8_t g_musashi_mem[MUSASHI_MEM_SIZE];

unsigned int m68k_read_memory_8(unsigned int addr)
{
    return (addr < MUSASHI_MEM_SIZE) ? g_musashi_mem[addr] : 0;
}
unsigned int m68k_read_memory_16(unsigned int addr)
{
    if (addr + 1 >= MUSASHI_MEM_SIZE) return 0;
    return ((unsigned int)g_musashi_mem[addr] << 8) | g_musashi_mem[addr + 1];
}
unsigned int m68k_read_memory_32(unsigned int addr)
{
    if (addr + 3 >= MUSASHI_MEM_SIZE) return 0;
    return ((unsigned int)g_musashi_mem[addr]     << 24) |
           ((unsigned int)g_musashi_mem[addr + 1] << 16) |
           ((unsigned int)g_musashi_mem[addr + 2] <<  8) |
            (unsigned int)g_musashi_mem[addr + 3];
}
void m68k_write_memory_8(unsigned int addr, unsigned int val)
{
    if (addr < MUSASHI_MEM_SIZE) g_musashi_mem[addr] = (uint8_t)val;
}
void m68k_write_memory_16(unsigned int addr, unsigned int val)
{
    if (addr + 1 < MUSASHI_MEM_SIZE) {
        g_musashi_mem[addr]     = (val >> 8) & 0xFF;
        g_musashi_mem[addr + 1] = val & 0xFF;
    }
}
void m68k_write_memory_32(unsigned int addr, unsigned int val)
{
    if (addr + 3 < MUSASHI_MEM_SIZE) {
        g_musashi_mem[addr]     = (val >> 24) & 0xFF;
        g_musashi_mem[addr + 1] = (val >> 16) & 0xFF;
        g_musashi_mem[addr + 2] = (val >>  8) & 0xFF;
        g_musashi_mem[addr + 3] =  val        & 0xFF;
    }
}

/* ======================================================================== */
/* Snapshot structs                                                           */
/* ======================================================================== */

typedef struct {
    uint32_t d[8];
    uint32_t a[7];   /* a0-a6 */
    uint32_t usp;
    uint32_t ssp;
    uint32_t pc;
    uint16_t sr;
} snap_t;

/* ======================================================================== */
/* JSON helpers (mirrors processor_tests.c)                                  */
/* ======================================================================== */

static uint32_t jnum(cJSON *item)
{
    if (!item || !cJSON_IsNumber(item)) return 0;
    return (uint32_t)(int64_t)item->valuedouble;
}

static void apply_ram_to_our_mem(cJSON *ram)
{
    if (!ram || !cJSON_IsArray(ram)) return;
    cJSON *pair;
    cJSON_ArrayForEach(pair, ram) {
        cJSON *a = cJSON_GetArrayItem(pair, 0);
        cJSON *v = cJSON_GetArrayItem(pair, 1);
        if (a && v) {
            uint32_t addr = jnum(a);
            if (addr < MEM_SIZE)
                mem_write8(addr, (uint8_t)(jnum(v) & 0xFF));
        }
    }
}

static void apply_ram_to_musashi_mem(cJSON *ram)
{
    if (!ram || !cJSON_IsArray(ram)) return;
    cJSON *pair;
    cJSON_ArrayForEach(pair, ram) {
        cJSON *a = cJSON_GetArrayItem(pair, 0);
        cJSON *v = cJSON_GetArrayItem(pair, 1);
        if (a && v) {
            uint32_t addr = jnum(a);
            if (addr < MUSASHI_MEM_SIZE)
                g_musashi_mem[addr] = (uint8_t)(jnum(v) & 0xFF);
        }
    }
}

/* ======================================================================== */
/* Apply initial state to our emulator                                        */
/* ======================================================================== */

static void our_apply_initial(cJSON *init)
{
    if (!init) return;
    for (int i = 0; i < 8; i++) {
        char k[3]; snprintf(k, sizeof(k), "d%d", i);
        cJSON *it = cJSON_GetObjectItem(init, k);
        if (it) cpu.d[i] = jnum(it);
    }
    for (int i = 0; i < 7; i++) {
        char k[3]; snprintf(k, sizeof(k), "a%d", i);
        cJSON *it = cJSON_GetObjectItem(init, k);
        if (it) cpu.a[i] = jnum(it);
    }
    cJSON *it;
    it = cJSON_GetObjectItem(init, "usp"); if (it) cpu.usp = jnum(it);
    it = cJSON_GetObjectItem(init, "ssp"); if (it) cpu.ssp = jnum(it);
    it = cJSON_GetObjectItem(init, "sr");  if (it) cpu.sr  = (uint16_t)(jnum(it) & 0xFFFF);

    /* Force supervisor mode for privileged instructions (mirrors processor_tests.c) */
    cJSON *prefetch = cJSON_GetObjectItem(init, "prefetch");
    if (prefetch && cJSON_IsArray(prefetch)) {
        cJSON *p0 = cJSON_GetArrayItem(prefetch, 0);
        if (p0 && cJSON_IsNumber(p0)) {
            uint32_t op = jnum(p0) & 0xFFFF;
            if (op == 0x007C || op == 0x027C || op == 0x0A7C ||
                (op & 0xFFC0) == 0x46C0 || (op & 0xFFC0) == 0x44C0 ||
                (op & 0xFFC0) == 0x42C0)
                cpu.sr |= 0x2000;
        }
    }

    it = cJSON_GetObjectItem(init, "pc"); if (it) cpu.pc = jnum(it);
    cpu.a[7] = (cpu.sr & 0x2000) ? cpu.ssp : cpu.usp;
    cpu.halted = 0;
    cpu.cycles = 0;

    apply_ram_to_our_mem(cJSON_GetObjectItem(init, "ram"));

    /* Write prefetch words to memory at PC */
    if (prefetch && cJSON_IsArray(prefetch)) {
        cJSON *p0 = cJSON_GetArrayItem(prefetch, 0);
        if (p0 && cJSON_IsNumber(p0)) {
            mem_write16(cpu.pc, (uint16_t)(jnum(p0) & 0xFFFF));
            cJSON *p1 = cJSON_GetArrayItem(prefetch, 1);
            if (p1 && cJSON_IsNumber(p1))
                mem_write16(cpu.pc + 2, (uint16_t)(jnum(p1) & 0xFFFF));
        }
    }
}

static void our_capture(snap_t *s)
{
    for (int i = 0; i < 8; i++) s->d[i] = cpu.d[i];
    for (int i = 0; i < 7; i++) s->a[i] = cpu.a[i];
    s->pc  = cpu.pc;
    s->sr  = cpu.sr & 0xFFFF;
    s->usp = cpu.usp;
    s->ssp = cpu.ssp;
}

/* ======================================================================== */
/* Apply initial state to Musashi                                             */
/* ======================================================================== */

static void musashi_apply_initial(cJSON *init)
{
    if (!init) return;

    /* SR first so Musashi knows which stack is active */
    uint16_t sr = 0x2700; /* default supervisor */
    cJSON *it = cJSON_GetObjectItem(init, "sr");
    if (it) sr = (uint16_t)(jnum(it) & 0xFFFF);

    /* Mirror the same privileged-instruction SR override */
    cJSON *prefetch = cJSON_GetObjectItem(init, "prefetch");
    if (prefetch && cJSON_IsArray(prefetch)) {
        cJSON *p0 = cJSON_GetArrayItem(prefetch, 0);
        if (p0 && cJSON_IsNumber(p0)) {
            uint32_t op = jnum(p0) & 0xFFFF;
            if (op == 0x007C || op == 0x027C || op == 0x0A7C ||
                (op & 0xFFC0) == 0x46C0 || (op & 0xFFC0) == 0x44C0 ||
                (op & 0xFFC0) == 0x42C0)
                sr |= 0x2000;
        }
    }
    m68k_set_reg(M68K_REG_SR, sr);

    /* Data registers */
    m68k_register_t dreg[8] = {
        M68K_REG_D0, M68K_REG_D1, M68K_REG_D2, M68K_REG_D3,
        M68K_REG_D4, M68K_REG_D5, M68K_REG_D6, M68K_REG_D7
    };
    for (int i = 0; i < 8; i++) {
        char k[3]; snprintf(k, sizeof(k), "d%d", i);
        it = cJSON_GetObjectItem(init, k);
        if (it) m68k_set_reg(dreg[i], jnum(it));
    }

    /* Address registers a0-a6 */
    m68k_register_t areg[7] = {
        M68K_REG_A0, M68K_REG_A1, M68K_REG_A2, M68K_REG_A3,
        M68K_REG_A4, M68K_REG_A5, M68K_REG_A6
    };
    for (int i = 0; i < 7; i++) {
        char k[3]; snprintf(k, sizeof(k), "a%d", i);
        it = cJSON_GetObjectItem(init, k);
        if (it) m68k_set_reg(areg[i], jnum(it));
    }

    /* Stack pointers: ISP = supervisor SP (A7 when S=1) */
    it = cJSON_GetObjectItem(init, "usp"); if (it) m68k_set_reg(M68K_REG_USP, jnum(it));
    it = cJSON_GetObjectItem(init, "ssp"); if (it) m68k_set_reg(M68K_REG_ISP, jnum(it));
    /* Set A7 to the active stack */
    {
        uint32_t usp_v = (uint32_t)m68k_get_reg(NULL, M68K_REG_USP);
        uint32_t isp_v = (uint32_t)m68k_get_reg(NULL, M68K_REG_ISP);
        m68k_set_reg(M68K_REG_A7, (sr & 0x2000) ? isp_v : usp_v);
    }

    it = cJSON_GetObjectItem(init, "pc"); if (it) m68k_set_reg(M68K_REG_PC, jnum(it));

    /* Memory */
    memset(g_musashi_mem, 0, MUSASHI_MEM_SIZE);
    apply_ram_to_musashi_mem(cJSON_GetObjectItem(init, "ram"));

    /* Prefetch → write instruction bytes to Musashi memory */
    if (prefetch && cJSON_IsArray(prefetch)) {
        uint32_t pc_val = (uint32_t)m68k_get_reg(NULL, M68K_REG_PC);
        cJSON *p0 = cJSON_GetArrayItem(prefetch, 0);
        if (p0 && cJSON_IsNumber(p0)) {
            uint16_t w0 = (uint16_t)(jnum(p0) & 0xFFFF);
            if (pc_val + 1 < MUSASHI_MEM_SIZE) {
                g_musashi_mem[pc_val]     = (w0 >> 8) & 0xFF;
                g_musashi_mem[pc_val + 1] =  w0       & 0xFF;
            }
            cJSON *p1 = cJSON_GetArrayItem(prefetch, 1);
            if (p1 && cJSON_IsNumber(p1)) {
                uint16_t w1 = (uint16_t)(jnum(p1) & 0xFFFF);
                if (pc_val + 3 < MUSASHI_MEM_SIZE) {
                    g_musashi_mem[pc_val + 2] = (w1 >> 8) & 0xFF;
                    g_musashi_mem[pc_val + 3] =  w1       & 0xFF;
                }
            }
        }
    }

    /* Clear any internal Musashi state left over from the previous test.
     * reset_cycles: set by m68k_pulse_reset(); if non-zero, m68k_execute()
     * eats those cycles first and may not execute any instruction.
     * stopped: a STOP instruction or halt from a prior test would block exec. */
    m68ki_cpu.reset_cycles = 0;
    m68ki_cpu.stopped = 0;
}

static void musashi_capture(snap_t *s)
{
    m68k_register_t dreg[8] = {
        M68K_REG_D0, M68K_REG_D1, M68K_REG_D2, M68K_REG_D3,
        M68K_REG_D4, M68K_REG_D5, M68K_REG_D6, M68K_REG_D7
    };
    m68k_register_t areg[7] = {
        M68K_REG_A0, M68K_REG_A1, M68K_REG_A2, M68K_REG_A3,
        M68K_REG_A4, M68K_REG_A5, M68K_REG_A6
    };
    for (int i = 0; i < 8; i++) s->d[i] = (uint32_t)m68k_get_reg(NULL, dreg[i]);
    for (int i = 0; i < 7; i++) s->a[i] = (uint32_t)m68k_get_reg(NULL, areg[i]);
    s->pc  = (uint32_t)m68k_get_reg(NULL, M68K_REG_PC);
    s->sr  = (uint16_t)(m68k_get_reg(NULL, M68K_REG_SR) & 0xFFFF);
    s->usp = (uint32_t)m68k_get_reg(NULL, M68K_REG_USP);
    /* Capture SSP: if in supervisor mode A7 is the SSP; otherwise use ISP */
    if (s->sr & 0x2000)
        s->ssp = (uint32_t)m68k_get_reg(NULL, M68K_REG_A7);
    else
        s->ssp = (uint32_t)m68k_get_reg(NULL, M68K_REG_ISP);
}

/* ======================================================================== */
/* Comparison                                                                 */
/* ======================================================================== */

/* Returns 1 if the two snapshots are identical, 0 if they differ.
 * Prints differences when printing is enabled.                              */
static int snaps_equal(const snap_t *a, const snap_t *b,
                       const char *label_a, const char *label_b,
                       const char *test_name, int print)
{
    int ok = 1;
    for (int i = 0; i < 8; i++) {
        if (a->d[i] != b->d[i]) {
            if (print)
                printf("    d%d: %s=0x%08X  %s=0x%08X\n",
                       i, label_a, a->d[i], label_b, b->d[i]);
            ok = 0;
        }
    }
    for (int i = 0; i < 7; i++) {
        if (a->a[i] != b->a[i]) {
            if (print)
                printf("    a%d: %s=0x%08X  %s=0x%08X\n",
                       i, label_a, a->a[i], label_b, b->a[i]);
            ok = 0;
        }
    }
    if (a->usp != b->usp) {
        if (print)
            printf("    usp: %s=0x%08X  %s=0x%08X\n",
                   label_a, a->usp, label_b, b->usp);
        ok = 0;
    }
    if (a->ssp != b->ssp) {
        if (print)
            printf("    ssp: %s=0x%08X  %s=0x%08X\n",
                   label_a, a->ssp, label_b, b->ssp);
        ok = 0;
    }
    if (a->pc != b->pc) {
        if (print)
            printf("    pc:  %s=0x%08X  %s=0x%08X\n",
                   label_a, a->pc, label_b, b->pc);
        ok = 0;
    }
    if (a->sr != b->sr) {
        if (print)
            printf("    sr:  %s=0x%04X       %s=0x%04X\n",
                   label_a, a->sr, label_b, b->sr);
        ok = 0;
    }
    (void)test_name;
    return ok;
}

/* Build a snap_t from the corpus "final" JSON node.
 * Returns 0 if the node is missing/invalid.                                 */
static int snap_from_json(cJSON *final, snap_t *s)
{
    if (!final || !cJSON_IsObject(final)) return 0;
    memset(s, 0, sizeof(*s));
    for (int i = 0; i < 8; i++) {
        char k[3]; snprintf(k, sizeof(k), "d%d", i);
        cJSON *it = cJSON_GetObjectItem(final, k);
        if (it) s->d[i] = jnum(it);
    }
    for (int i = 0; i < 7; i++) {
        char k[3]; snprintf(k, sizeof(k), "a%d", i);
        cJSON *it = cJSON_GetObjectItem(final, k);
        if (it) s->a[i] = jnum(it);
    }
    cJSON *it;
    it = cJSON_GetObjectItem(final, "usp"); if (it) s->usp = jnum(it);
    it = cJSON_GetObjectItem(final, "ssp"); if (it) s->ssp = jnum(it);
    it = cJSON_GetObjectItem(final, "pc");  if (it) s->pc  = jnum(it);
    it = cJSON_GetObjectItem(final, "sr");  if (it) s->sr  = (uint16_t)(jnum(it) & 0xFFFF);
    return 1;
}

/* Compare RAM bytes listed in corpus "final.ram" against a flat buffer.
 * Returns number of bytes that differ.                                      */
static int compare_ram(cJSON *final_ram, const uint8_t *buf, size_t buf_size,
                       const char *label, const char *test_name, int print,
                       int max_print)
{
    if (!final_ram || !cJSON_IsArray(final_ram)) return 0;
    int diffs = 0;
    cJSON *pair;
    cJSON_ArrayForEach(pair, final_ram) {
        cJSON *a = cJSON_GetArrayItem(pair, 0);
        cJSON *v = cJSON_GetArrayItem(pair, 1);
        if (!a || !v) continue;
        uint32_t addr = jnum(a);
        uint8_t  exp  = (uint8_t)(jnum(v) & 0xFF);
        uint8_t  got  = (addr < buf_size) ? buf[addr] : 0;
        if (got != exp) {
            diffs++;
            if (print && diffs <= max_print)
                printf("    ram[0x%04X]: expected 0x%02X  %s=0x%02X\n",
                       addr, exp, label, got);
        }
    }
    (void)test_name;
    return diffs;
}

/* ======================================================================== */
/* File loading (copied from processor_tests.c)                              */
/* ======================================================================== */

static char *load_file(const char *path, size_t *out_size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 100 * 1024 * 1024) { fclose(f); return NULL; }
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = '\0';
    *out_size = n;
    return buf;
}

#ifdef HAVE_ZLIB
static char *load_gz(const char *path, size_t *out_size)
{
    gzFile f = gzopen(path, "rb");
    if (!f) return NULL;
    size_t cap = 65536, len = 0;
    char *buf = malloc(cap + 1);
    if (!buf) { gzclose(f); return NULL; }
    int c;
    while ((c = gzgetc(f)) != -1) {
        if (len >= cap) {
            cap *= 2;
            char *nb = realloc(buf, cap + 1);
            if (!nb) { free(buf); gzclose(f); return NULL; }
            buf = nb;
        }
        buf[len++] = (char)c;
    }
    buf[len] = '\0';
    gzclose(f);
    *out_size = len;
    return buf;
}
#endif

static char *load_json(const char *path, size_t *out_size)
{
    size_t len = strlen(path);
#ifdef HAVE_ZLIB
    if (len > 3 && strcmp(path + len - 3, ".gz") == 0)
        return load_gz(path, out_size);
#endif
    return load_file(path, out_size);
}

/* ======================================================================== */
/* Per-file runner                                                            */
/* ======================================================================== */

static int run_file(const char *path,
                    long *pass, long *corpus_diff, long *real_diff,
                    int verbose)
{
    size_t json_size;
    char *json = load_json(path, &json_size);
    if (!json) { fprintf(stderr, "Cannot load %s\n", path); return -1; }

    cJSON *root = cJSON_ParseWithLength(json, json_size);
    free(json);
    if (!root) {
        fprintf(stderr, "JSON parse error in %s\n", path);
        return -1;
    }
    if (!cJSON_IsArray(root)) { cJSON_Delete(root); return -1; }

    int count = cJSON_GetArraySize(root);

    /* Read RAM array from the final node once per test to save allocations. */
    for (int i = 0; i < count; i++) {
        cJSON *test = cJSON_GetArrayItem(root, i);
        if (!test || !cJSON_IsObject(test)) continue;

        cJSON *name_item = cJSON_GetObjectItem(test, "name");
        const char *name = (name_item && cJSON_IsString(name_item))
                         ? name_item->valuestring : "?";

        cJSON *initial = cJSON_GetObjectItem(test, "initial");
        cJSON *final   = cJSON_GetObjectItem(test, "final");
        if (!initial || !final) continue;

        /* --- Run our emulator ------------------------------------------ */
        mem_reset();
        our_apply_initial(initial);
        cpu_step();
        snap_t our_snap;
        our_capture(&our_snap);
        /* Save a copy of our memory for RAM comparison */
        /* (we'll compare specific addresses from final.ram below) */

        /* --- Run Musashi ------------------------------------------------ */
        musashi_apply_initial(initial);
        m68k_execute(1);   /* 1-cycle budget → executes exactly one instruction */
        snap_t mus_snap;
        musashi_capture(&mus_snap);

        /* --- Compare our result vs Musashi ----------------------------- */
        int regs_match_mus = snaps_equal(&our_snap, &mus_snap, NULL, NULL, name, 0);

        /* RAM comparison: check final.ram addresses in our memory vs Musashi */
        cJSON *final_ram = cJSON_GetObjectItem(final, "ram");
        int our_ram_ok = 1, mus_ram_ok = 1;
        if (final_ram && cJSON_IsArray(final_ram)) {
            cJSON *pair;
            cJSON_ArrayForEach(pair, final_ram) {
                cJSON *a = cJSON_GetArrayItem(pair, 0);
                cJSON *v = cJSON_GetArrayItem(pair, 1);
                if (!a || !v) continue;
                uint32_t addr = jnum(a);
                uint8_t  exp  = (uint8_t)(jnum(v) & 0xFF);
                uint8_t  our  = (addr < MEM_SIZE)            ? mem_read8(addr)     : 0;
                uint8_t  mus  = (addr < MUSASHI_MEM_SIZE)    ? g_musashi_mem[addr] : 0;
                if (our != mus) regs_match_mus = 0;  /* fold RAM into agreement check */
                if (our != exp) our_ram_ok = 0;
                if (mus != exp) mus_ram_ok = 0;
            }
        }

        /* Build corpus snapshot for corpus-agreement check */
        snap_t corp_snap;
        int have_corpus = snap_from_json(final, &corp_snap);
        int our_match_corpus  = have_corpus && snaps_equal(&our_snap,  &corp_snap, NULL, NULL, name, 0) && our_ram_ok;
        int mus_match_corpus  = have_corpus && snaps_equal(&mus_snap,  &corp_snap, NULL, NULL, name, 0) && mus_ram_ok;
        (void)mus_match_corpus;

        if (regs_match_mus) {
            if (our_match_corpus) {
                (*pass)++;
            } else {
                (*corpus_diff)++;
                if (verbose >= 2) {
                    printf("CORPUS_DIFF %s\n", name);
                    snaps_equal(&our_snap, &corp_snap, "ours", "corpus", name, 1);
                }
            }
        } else {
            (*real_diff)++;
            /* Always print REAL_DIFF details */
            printf("REAL_DIFF  %s\n", name);
            snaps_equal(&our_snap, &mus_snap, "ours ", "mushi", name, 1);
            if (final_ram && cJSON_IsArray(final_ram)) {
                /* Print any RAM byte mismatches between ours and Musashi */
                cJSON *pair;
                int printed = 0;
                cJSON_ArrayForEach(pair, final_ram) {
                    cJSON *a = cJSON_GetArrayItem(pair, 0);
                    cJSON *v = cJSON_GetArrayItem(pair, 1);
                    if (!a || !v) continue;
                    uint32_t addr = jnum(a);
                    uint8_t our_b = (addr < MEM_SIZE)         ? mem_read8(addr)     : 0;
                    uint8_t mus_b = (addr < MUSASHI_MEM_SIZE) ? g_musashi_mem[addr] : 0;
                    if (our_b != mus_b && printed < 8) {
                        printf("    ram[0x%04X]: ours=0x%02X  mushi=0x%02X\n",
                               addr, our_b, mus_b);
                        printed++;
                    }
                }
            }
        }
    }

    cJSON_Delete(root);
    return 0;
}

/* ======================================================================== */
/* Directory runner                                                           */
/* ======================================================================== */

static int name_matches_filter(const char *name, const char *filter)
{
    if (!filter || !*filter) return 1;
    return strstr(name, filter) != NULL;
}

static void run_directory(const char *dir, const char *filter,
                          long *pass, long *corpus_diff, long *real_diff,
                          int verbose)
{
    DIR *d = opendir(dir);
    if (!d) { perror(dir); return; }

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        size_t len = strlen(ent->d_name);
        int is_json = (len > 5  && strcmp(ent->d_name + len - 5,  ".json")    == 0);
#ifdef HAVE_ZLIB
        int is_gz   = (len > 8  && strcmp(ent->d_name + len - 8,  ".json.gz") == 0);
        if (!is_json && !is_gz) continue;
#else
        int is_gz = 0;
        if (!is_json) continue;
#endif
        /* Strip extension for filter */
        char base[256];
        size_t blen = len - (is_gz ? 8 : 5);
        if (blen >= sizeof(base)) continue;
        memcpy(base, ent->d_name, blen); base[blen] = '\0';
        if (!name_matches_filter(base, filter)) continue;

        char path[512];
        snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
        run_file(path, pass, corpus_diff, real_diff, verbose);
    }
    closedir(d);
}

/* ======================================================================== */
/* Entry point                                                                */
/* ======================================================================== */

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <test-dir> [filter] [-v|-vv]\n", argv[0]);
        fprintf(stderr, "  filter    only run files whose name contains this string\n");
        fprintf(stderr, "  -v        also print CORPUS_DIFF cases\n");
        fprintf(stderr, "  -vv       (implied by -v) same as -v\n");
        return 1;
    }

    const char *dir    = argv[1];
    const char *filter = NULL;
    int verbose = 0;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-vv") == 0) verbose = 2;
        else if (strcmp(argv[i], "-v") == 0 && verbose < 1) verbose = 1;
        else filter = argv[i];
    }

    /* Initialise our emulator */
    mem_init();
    cpu_init(CPU_MODEL_68000);

    /* Initialise Musashi */
    m68k_set_cpu_type(M68K_CPU_TYPE_68000);
    m68k_init();
    memset(g_musashi_mem, 0, sizeof(g_musashi_mem));
    m68k_pulse_reset();  /* required once to initialise internal state machine */

    printf("Musashi diff: %s%s%s%s\n", dir,
           filter ? " (filter: " : "",
           filter ? filter       : "",
           filter ? ")"          : "");
    printf("  REAL_DIFF  = we disagree with Musashi (likely a bug in our emulator)\n");
    printf("  CORPUS_DIFF= we agree with Musashi but differ from the JSON corpus\n");
    printf("               (likely a test-suite convention, not an emulator bug)\n\n");

    long pass = 0, corpus_diff = 0, real_diff = 0;
    run_directory(dir, filter, &pass, &corpus_diff, &real_diff, verbose);

    printf("\n");
    printf("Results:\n");
    printf("  PASS        : %ld\n", pass);
    printf("  CORPUS_DIFF : %ld  (we match Musashi; use -v to see details)\n", corpus_diff);
    printf("  REAL_DIFF   : %ld  (we disagree with Musashi)\n", real_diff);
    printf("  Total       : %ld\n", pass + corpus_diff + real_diff);

    return (real_diff > 0) ? 1 : 0;
}

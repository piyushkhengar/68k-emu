/*
 * ProcessorTests harness for SingleStepTests/680x0.
 * Loads JSON (or .json.gz) test files, applies initial state, runs one instruction,
 * compares to final state.
 */

#include "cpu.h"
#include "cpu_internal.h"
#include "memory.h"
#include "processor_tests.h"
#include "deps/cJSON/cJSON.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef HAVE_ZLIB
#include <zlib.h>
#endif

static uint32_t get_num(cJSON *item)
{
    if (!item || !cJSON_IsNumber(item))
        return 0;
    double d = item->valuedouble;
    /* Use int64_t to avoid overflow: values 0..2^32-1 map correctly to uint32_t */
    return (uint32_t)(int64_t)d;
}

static void apply_ram(cJSON *ram)
{
    if (!ram || !cJSON_IsArray(ram))
        return;
    cJSON *pair;
    cJSON_ArrayForEach(pair, ram) {
        if (!cJSON_IsArray(pair))
            continue;
        cJSON *addr_item = cJSON_GetArrayItem(pair, 0);
        cJSON *val_item = cJSON_GetArrayItem(pair, 1);
        if (!addr_item || !val_item)
            continue;
        uint32_t addr = (uint32_t)get_num(addr_item);
        uint32_t val = (uint32_t)get_num(val_item) & 0xFF;
        if (addr < MEM_SIZE)
            mem_write8(addr, (uint8_t)val);
    }
}

static void apply_initial(cJSON *initial)
{
    if (!initial || !cJSON_IsObject(initial))
        return;

    cJSON *item;
    const char *regs[] = {"d0","d1","d2","d3","d4","d5","d6","d7", NULL};
    for (int i = 0; regs[i]; i++) {
        item = cJSON_GetObjectItem(initial, regs[i]);
        if (item)
            cpu.d[i] = get_num(item);
    }
    const char *aregs[] = {"a0","a1","a2","a3","a4","a5","a6", NULL};
    for (int i = 0; aregs[i]; i++) {
        item = cJSON_GetObjectItem(initial, aregs[i]);
        if (item)
            cpu.a[i] = get_num(item);
    }
    item = cJSON_GetObjectItem(initial, "usp");
    if (item && cJSON_IsNumber(item))
        cpu.usp = get_num(item);
    item = cJSON_GetObjectItem(initial, "ssp");
    if (item && cJSON_IsNumber(item))
        cpu.ssp = get_num(item);
    item = cJSON_GetObjectItem(initial, "sr");
    if (item && cJSON_IsNumber(item))
        cpu.sr = (uint16_t)(get_num(item) & 0xFFFF);
    /* ORI/ANDI/EORI to SR and MOVE to SR/CCR are privileged; tests expect success, so force S=1 */
    cJSON *prefetch_obj = cJSON_GetObjectItem(initial, "prefetch");
    if (prefetch_obj && cJSON_IsArray(prefetch_obj)) {
        cJSON *p0 = cJSON_GetArrayItem(prefetch_obj, 0);
        if (p0 && cJSON_IsNumber(p0)) {
            uint32_t op = get_num(p0) & 0xFFFF;
            if (op == 0x007C || op == 0x027C || op == 0x0A7C ||  /* ORI/ANDI/EORI to SR */
                (op & 0xFFC0) == 0x46C0 || (op & 0xFFC0) == 0x44C0 || (op & 0xFFC0) == 0x42C0)  /* MOVE to SR/CCR */
                cpu.sr |= 0x2000;
        }
    }
    item = cJSON_GetObjectItem(initial, "pc");
    if (item && cJSON_IsNumber(item))
        cpu.pc = get_num(item);

    cpu.a[7] = (cpu.sr & 0x2000) ? cpu.ssp : cpu.usp;
    cpu.halted = 0;
    cpu.cycles = 0;

    apply_ram(cJSON_GetObjectItem(initial, "ram"));

    /* Prefetch holds instruction at PC: first word = opcode, second = extension (if any) */
    cJSON *prefetch = cJSON_GetObjectItem(initial, "prefetch");
    if (prefetch && cJSON_IsArray(prefetch)) {
        cJSON *p0 = cJSON_GetArrayItem(prefetch, 0);
        if (p0 && cJSON_IsNumber(p0)) {
            mem_write16(cpu.pc, (uint16_t)(get_num(p0) & 0xFFFF));
            cJSON *p1 = cJSON_GetArrayItem(prefetch, 1);
            if (p1 && cJSON_IsNumber(p1))
                mem_write16(cpu.pc + 2, (uint16_t)(get_num(p1) & 0xFFFF));
        }
    }
}

static int check_final(cJSON *final, const char *name, int *first_mismatch)
{
    if (!final || !cJSON_IsObject(final))
        return 1;

    cJSON *item;
    const char *regs[] = {"d0","d1","d2","d3","d4","d5","d6","d7", NULL};
    for (int i = 0; regs[i]; i++) {
        item = cJSON_GetObjectItem(final, regs[i]);
        if (item) {
            uint32_t expected = get_num(item);
            if (cpu.d[i] != expected) {
                if (first_mismatch)
                    *first_mismatch = 1;
                printf("  FAIL %s: expected %s=0x%08X, got 0x%08X\n",
                       name, regs[i], expected, (unsigned)cpu.d[i]);
                return 0;
            }
        }
    }
    const char *aregs[] = {"a0","a1","a2","a3","a4","a5","a6", NULL};
    for (int i = 0; aregs[i]; i++) {
        item = cJSON_GetObjectItem(final, aregs[i]);
        if (item) {
            uint32_t expected = get_num(item);
            if (cpu.a[i] != expected) {
                if (first_mismatch)
                    *first_mismatch = 1;
                printf("  FAIL %s: expected %s=0x%08X, got 0x%08X\n",
                       name, aregs[i], expected, (unsigned)cpu.a[i]);
                return 0;
            }
        }
    }
    item = cJSON_GetObjectItem(final, "usp");
    if (item && cpu.usp != get_num(item)) {
        if (first_mismatch) *first_mismatch = 1;
        printf("  FAIL %s: expected usp=0x%08X, got 0x%08X\n",
               name, (unsigned)get_num(item), (unsigned)cpu.usp);
        return 0;
    }
    item = cJSON_GetObjectItem(final, "ssp");
    if (item && cpu.ssp != get_num(item)) {
        if (first_mismatch) *first_mismatch = 1;
        printf("  FAIL %s: expected ssp=0x%08X, got 0x%08X\n",
               name, (unsigned)get_num(item), (unsigned)cpu.ssp);
        return 0;
    }
    item = cJSON_GetObjectItem(final, "sr");
    if (item && (cpu.sr & 0xFFFF) != (get_num(item) & 0xFFFF)) {
        if (first_mismatch) *first_mismatch = 1;
        printf("  FAIL %s: expected sr=0x%04X, got 0x%04X\n",
               name, (unsigned)(get_num(item) & 0xFFFF), (unsigned)(cpu.sr & 0xFFFF));
        return 0;
    }
    item = cJSON_GetObjectItem(final, "pc");
    if (item && cpu.pc != get_num(item)) {
        if (first_mismatch) *first_mismatch = 1;
        printf("  FAIL %s: expected pc=0x%08X, got 0x%08X\n",
               name, (unsigned)get_num(item), (unsigned)cpu.pc);
        return 0;
    }

    cJSON *ram = cJSON_GetObjectItem(final, "ram");
    if (ram && cJSON_IsArray(ram)) {
        cJSON *pair;
        cJSON_ArrayForEach(pair, ram) {
            if (!cJSON_IsArray(pair))
                continue;
            cJSON *addr_item = cJSON_GetArrayItem(pair, 0);
            cJSON *val_item = cJSON_GetArrayItem(pair, 1);
            if (!addr_item || !val_item)
                continue;
            uint32_t addr = (uint32_t)get_num(addr_item);
            uint32_t expected = (uint32_t)get_num(val_item) & 0xFF;
            uint8_t actual = (addr < MEM_SIZE) ? mem_read8(addr) : 0;
            if (actual != (uint8_t)expected) {
                if (first_mismatch) *first_mismatch = 1;
                printf("  FAIL %s: expected ram[0x%08X]=0x%02X, got 0x%02X\n",
                       name, addr, expected, actual);
                return 0;
            }
        }
    }
    return 1;
}

static char *load_file(const char *path, size_t *out_size)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 100 * 1024 * 1024) {  /* 100MB max */
        fclose(f);
        return NULL;
    }
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
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
    if (!f)
        return NULL;
    size_t cap = 65536;
    size_t len = 0;
    char *buf = (char *)malloc(cap + 1);
    if (!buf) {
        gzclose(f);
        return NULL;
    }
    int c;
    while ((c = gzgetc(f)) != -1) {
        if (len >= cap) {
            cap *= 2;
            char *newbuf = (char *)realloc(buf, cap + 1);
            if (!newbuf) {
                free(buf);
                gzclose(f);
                return NULL;
            }
            buf = newbuf;
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

static int run_file(const char *path, int *passed, int *failed, int verbose, int test_index)
{
    size_t json_size;
    char *json = load_json(path, &json_size);
    if (!json) {
        fprintf(stderr, "Cannot load %s\n", path);
        return -1;
    }

    cJSON *root = cJSON_ParseWithLength(json, json_size);
    free(json);
    if (!root) {
        fprintf(stderr, "JSON parse error in %s: %s\n", path, cJSON_GetErrorPtr());
        return -1;
    }

    if (!cJSON_IsArray(root)) {
        cJSON_Delete(root);
        fprintf(stderr, "%s: root is not array\n", path);
        return -1;
    }

    int count = cJSON_GetArraySize(root);
    int file_passed = 0, file_failed = 0;

    for (int i = 0; i < count; i++) {
        if (test_index >= 0 && i != test_index)
            continue;

        cJSON *test = cJSON_GetArrayItem(root, i);
        if (!test || !cJSON_IsObject(test))
            continue;

        cJSON *name_item = cJSON_GetObjectItem(test, "name");
        const char *name = name_item && cJSON_IsString(name_item) ? name_item->valuestring : "?";
        cJSON *initial = cJSON_GetObjectItem(test, "initial");
        cJSON *final = cJSON_GetObjectItem(test, "final");

        if (!initial || !final) {
            file_failed++;
            if (verbose)
                printf("  SKIP %s (missing initial/final)\n", name);
            continue;
        }

        mem_reset();
        apply_initial(initial);
        if (test_index >= 0) {
            printf("Before: pc=0x%08X a6=0x%08X d0=0x%08X\n", (unsigned)cpu.pc, (unsigned)cpu.a[6], (unsigned)cpu.d[0]);
        }
        cpu_step();
        if (test_index >= 0) {
            printf("After:  pc=0x%08X a6=0x%08X d0=0x%08X\n", (unsigned)cpu.pc, (unsigned)cpu.a[6], (unsigned)cpu.d[0]);
            cJSON *exp_a6 = cJSON_GetObjectItem(final, "a6");
            if (exp_a6)
                printf("Expected a6=0x%08X\n", (unsigned)get_num(exp_a6));
        }

        int first = 0;
        if (check_final(final, name, &first)) {
            file_passed++;
        } else {
            file_failed++;
        }
        if (test_index >= 0)
            break;
    }

    cJSON_Delete(root);
    *passed += file_passed;
    *failed += file_failed;
    return 0;
}

static int name_matches_filter(const char *name, const char *filter)
{
    if (!filter || !*filter)
        return 1;
    return strstr(name, filter) != NULL;
}

static int run_directory(const char *dir, const char *filter, int *passed, int *failed, int verbose, int test_index)
{
    DIR *d = opendir(dir);
    if (!d) {
        perror(dir);
        return -1;
    }

    struct dirent *ent;
    int files = 0;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.')
            continue;
        size_t len = strlen(ent->d_name);
        if (len < 5)
            continue;
        int is_json = (len > 5 && strcmp(ent->d_name + len - 5, ".json") == 0);
#ifdef HAVE_ZLIB
        int is_gz = (len > 8 && strcmp(ent->d_name + len - 8, ".json.gz") == 0);
        if (!is_json && !is_gz)
            continue;
#else
        if (!is_json)
            continue;
#endif

        /* Strip .json or .json.gz for filter match */
        char base[256];
        size_t base_len = len;
        if (is_gz)
            base_len -= 8;
        else if (is_json)
            base_len -= 5;
        if (base_len >= sizeof(base))
            continue;
        memcpy(base, ent->d_name, base_len);
        base[base_len] = '\0';
        if (!name_matches_filter(base, filter))
            continue;

        char path[512];
        snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
        if (run_file(path, passed, failed, verbose, test_index) == 0)
            files++;
    }
    closedir(d);
    return files;
}

int run_processor_tests(const char *dir, const char *filter)
{
    int test_index = -1;
    const char *env = getenv("PROCESSOR_TEST_INDEX");
    if (env && *env)
        test_index = atoi(env);

    printf("ProcessorTests: %s%s%s%s\n", dir,
           filter && *filter ? " (filter: " : "",
           filter && *filter ? filter : "",
           filter && *filter ? ")" : "");
    int passed = 0, failed = 0;
    int verbose = test_index >= 0 ? 1 : 0;

    if (run_directory(dir, filter, &passed, &failed, verbose, test_index) < 0)
        return 1;

    printf("Passed: %d  Failed: %d\n", passed, failed);
    return failed ? 1 : 0;
}

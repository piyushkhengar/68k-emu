/* bare-metal/include/bare.h
 * Master bare-metal shim.
 * Included automatically via -I bare-metal/include before all system headers
 * (which don't exist under -nostdinc anyway).
 * Provides: malloc/free/calloc/realloc, printf/fprintf, memset/memcpy/etc,
 *           and the VGA kprintf declaration.
 */

#ifndef BARE_H
#define BARE_H

/* Freestanding-safe types — provided by the compiler itself, not libc */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>

/* ---- Memory allocation --------------------------------------------------- */
void *bm_malloc(size_t n);
void  bm_free(void *p);
void *bm_calloc(size_t nmemb, size_t size);
void *bm_realloc(void *p, size_t new_size);

#define malloc(n)         bm_malloc(n)
#define free(p)           bm_free(p)
#define calloc(n, s)      bm_calloc((n), (s))
#define realloc(p, s)     bm_realloc((p), (s))

/* ---- String / memory operations ------------------------------------------ */
void  *bm_memset(void *s, int c, size_t n);
void  *bm_memcpy(void *dst, const void *src, size_t n);
void  *bm_memmove(void *dst, const void *src, size_t n);
int    bm_memcmp(const void *a, const void *b, size_t n);
size_t bm_strlen(const char *s);
int    bm_strcmp(const char *a, const char *b);
int    bm_strncmp(const char *a, const char *b, size_t n);
char  *bm_strcpy(char *dst, const char *src);
char  *bm_strncpy(char *dst, const char *src, size_t n);
char  *bm_strcat(char *dst, const char *src);
char  *bm_strncat(char *dst, const char *src, size_t n);
char  *bm_strchr(const char *s, int c);
char  *bm_strrchr(const char *s, int c);
char  *bm_strstr(const char *haystack, const char *needle);
char  *bm_strdup(const char *s);
long   bm_strtol(const char *s, char **end, int base);
unsigned long bm_strtoul(const char *s, char **end, int base);
double bm_strtod(const char *s, char **end);
int    bm_atoi(const char *s);

#define memset(s,c,n)     bm_memset((s),(c),(n))
#define memcpy(d,s,n)     bm_memcpy((d),(s),(n))
#define memmove(d,s,n)    bm_memmove((d),(s),(n))
#define memcmp(a,b,n)     bm_memcmp((a),(b),(n))
#define strlen(s)         bm_strlen(s)
#define strcmp(a,b)       bm_strcmp((a),(b))
#define strncmp(a,b,n)    bm_strncmp((a),(b),(n))
#define strcpy(d,s)       bm_strcpy((d),(s))
#define strncpy(d,s,n)    bm_strncpy((d),(s),(n))
#define strcat(d,s)       bm_strcat((d),(s))
#define strncat(d,s,n)    bm_strncat((d),(s),(n))
#define strchr(s,c)       bm_strchr((s),(c))
#define strrchr(s,c)      bm_strrchr((s),(c))
#define strstr(h,n)       bm_strstr((h),(n))
#define strdup(s)         bm_strdup(s)
#define strtol(s,e,b)     bm_strtol((s),(e),(b))
#define strtoul(s,e,b)    bm_strtoul((s),(e),(b))
#define strtod(s,e)       bm_strtod((s),(e))
#define atoi(s)           bm_atoi(s)

/* ---- Output -------------------------------------------------------------- */
int  kprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int  kvprintf(const char *fmt, va_list ap);
void vga_putchar(char c);
void vga_clear(void);

/* stdio.h shims */
#define printf(...)       kprintf(__VA_ARGS__)
#define fprintf(f, ...)   kprintf(__VA_ARGS__)
/* stderr/stdout — ignored; fprintf just goes to kprintf */
#define stderr            ((void *)0)
#define stdout            ((void *)0)
#define EOF               (-1)

/* ---- Abort / exit -------------------------------------------------------- */
void bm_abort(void) __attribute__((noreturn));
#define abort()   bm_abort()
#define exit(n)   bm_abort()
#define assert(e) do { if (!(e)) bm_abort(); } while (0)

/* ---- Math functions ------------------------------------------------------ */
static inline double bm_fabs(double x)  { return x < 0 ? -x : x; }
static inline double bm_floor(double x) {
    long i = (long)x;
    return (double)(i - (x < 0 && x != (double)i));
}
static inline double bm_ceil(double x) {
    long i = (long)x;
    return (double)(i + (x > 0 && x != (double)i));
}
static inline double bm_pow(double b, double e) {
    /* integer exponents only — sufficient for cJSON number formatting */
    double r = 1.0;
    int n = (int)e;
    for (int i = 0; i < n; i++) r *= b;
    return r;
}
/* Full math functions (implementations in libbm.c) */
double bm_sin(double x);
double bm_cos(double x);
double bm_log(double x);
double bm_log2(double x);
double bm_sqrt(double x);
double bm_exp(double x);

#define fabs(x)   bm_fabs(x)
#define floor(x)  bm_floor(x)
#define ceil(x)   bm_ceil(x)
#define pow(b,e)  bm_pow((b),(e))
/* Note: sin/cos/log/log2/sqrt/exp are defined in math.h shadow header.
 * If math.h was not included, the bm_ functions are still available directly. */

/* ---- Misc stdlib shims --------------------------------------------------- */
#define NULL ((void *)0)
/* qsort stub — not needed for the emulator core */
static inline void bm_qsort(void *b, size_t n, size_t s,
                             int (*cmp)(const void *, const void *)) {
    /* insertion sort — sufficient for tiny arrays */
    char *base = (char *)b;
    for (size_t i = 1; i < n; i++) {
        for (size_t j = i; j > 0 && cmp(base+(j-1)*s, base+j*s) > 0; j--) {
            char *a = base+(j-1)*s, *bb2 = base+j*s;
            for (size_t k = 0; k < s; k++) { char t=a[k]; a[k]=bb2[k]; bb2[k]=t; }
        }
    }
}
#define qsort(b,n,s,c) bm_qsort((b),(n),(s),(c))

#endif /* BARE_H */

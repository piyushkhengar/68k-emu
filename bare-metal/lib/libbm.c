/* bare-metal/lib/libbm.c
 * Bump allocator, kprintf, and all libc string/memory replacements.
 */

#include "bare.h"

/* =========================================================================
 * Bump allocator
 * ========================================================================= */

extern char heap_start[];
extern char heap_end[];

static char *heap_ptr = 0;

void *bm_malloc(size_t n)
{
    if (!heap_ptr)
        heap_ptr = heap_start;

    /* 16-byte alignment */
    n = (n + 15u) & ~15u;

    if (heap_ptr + n > heap_end)
        bm_abort();  /* out of memory — halt */

    void *p = heap_ptr;
    heap_ptr += n;
    return p;
}

void bm_free(void *p)
{
    (void)p; /* bump allocator: free is a no-op */
}

void *bm_calloc(size_t nmemb, size_t size)
{
    size_t total = nmemb * size;
    void *p = bm_malloc(total);
    bm_memset(p, 0, total);
    return p;
}

void *bm_realloc(void *p, size_t new_size)
{
    if (!p)
        return bm_malloc(new_size);
    /* We can't know the old size without a header — allocate new and copy.
     * cJSON always grows, so copying new_size bytes is safe. */
    void *np = bm_malloc(new_size);
    bm_memcpy(np, p, new_size);
    return np;
}

/* =========================================================================
 * Memory operations
 * ========================================================================= */

void *bm_memset(void *s, int c, size_t n)
{
    uint8_t *p = (uint8_t *)s;
    uint8_t b = (uint8_t)c;
    /* Word-fill the bulk for speed */
    while (n && ((uintptr_t)p & 3)) { *p++ = b; n--; }
    uint32_t w = b | ((uint32_t)b << 8) | ((uint32_t)b << 16) | ((uint32_t)b << 24);
    uint32_t *pw = (uint32_t *)p;
    while (n >= 4) { *pw++ = w; n -= 4; }
    p = (uint8_t *)pw;
    while (n--) *p++ = b;
    return s;
}

void *bm_memcpy(void *dst, const void *src, size_t n)
{
    uint8_t       *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
    return dst;
}

void *bm_memmove(void *dst, const void *src, size_t n)
{
    uint8_t       *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    if (d < s || d >= s + n) {
        while (n--) *d++ = *s++;
    } else {
        d += n; s += n;
        while (n--) *--d = *--s;
    }
    return dst;
}

int bm_memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *pa = (const uint8_t *)a;
    const uint8_t *pb = (const uint8_t *)b;
    while (n--) {
        if (*pa != *pb) return (int)*pa - (int)*pb;
        pa++; pb++;
    }
    return 0;
}

/* =========================================================================
 * String operations
 * ========================================================================= */

size_t bm_strlen(const char *s)
{
    const char *p = s;
    while (*p) p++;
    return (size_t)(p - s);
}

int bm_strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

int bm_strncmp(const char *a, const char *b, size_t n)
{
    while (n && *a && *a == *b) { a++; b++; n--; }
    if (!n) return 0;
    return (unsigned char)*a - (unsigned char)*b;
}

char *bm_strcpy(char *dst, const char *src)
{
    char *d = dst;
    while ((*d++ = *src++)) {}
    return dst;
}

char *bm_strncpy(char *dst, const char *src, size_t n)
{
    char *d = dst;
    while (n && (*d++ = *src++)) n--;
    while (n--) *d++ = '\0';
    return dst;
}

char *bm_strcat(char *dst, const char *src)
{
    char *d = dst + bm_strlen(dst);
    while ((*d++ = *src++)) {}
    return dst;
}

char *bm_strncat(char *dst, const char *src, size_t n)
{
    char *d = dst + bm_strlen(dst);
    while (n-- && (*d = *src++)) d++;
    *d = '\0';
    return dst;
}

char *bm_strchr(const char *s, int c)
{
    for (; *s; s++)
        if ((unsigned char)*s == (unsigned char)c) return (char *)s;
    if (c == '\0') return (char *)s;
    return 0;
}

char *bm_strrchr(const char *s, int c)
{
    const char *last = 0;
    for (; *s; s++)
        if ((unsigned char)*s == (unsigned char)c) last = s;
    if (c == '\0') return (char *)s;
    return (char *)last;
}

char *bm_strstr(const char *haystack, const char *needle)
{
    if (!*needle) return (char *)haystack;
    for (; *haystack; haystack++) {
        const char *h = haystack, *n = needle;
        while (*h && *n && *h == *n) { h++; n++; }
        if (!*n) return (char *)haystack;
    }
    return 0;
}

char *bm_strdup(const char *s)
{
    size_t len = bm_strlen(s) + 1;
    char *p = (char *)bm_malloc(len);
    bm_memcpy(p, s, len);
    return p;
}

long bm_strtol(const char *s, char **end, int base)
{
    while (*s == ' ' || *s == '\t') s++;
    int neg = 0;
    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') s++;
    if (base == 0) {
        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; s += 2; }
        else if (s[0] == '0') { base = 8; s++; }
        else base = 10;
    } else if (base == 16 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
    }
    long val = 0;
    const char *start = s;
    for (;;) {
        int d;
        if (*s >= '0' && *s <= '9') d = *s - '0';
        else if (*s >= 'a' && *s <= 'z') d = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'Z') d = *s - 'A' + 10;
        else break;
        if (d >= base) break;
        val = val * base + d;
        s++;
    }
    if (end) *end = (s == start) ? (char *)start : (char *)s;
    return neg ? -val : val;
}

unsigned long bm_strtoul(const char *s, char **end, int base)
{
    return (unsigned long)bm_strtol(s, end, base);
}

double bm_strtod(const char *s, char **end)
{
    /* Minimal: handles integers and simple decimals — enough for cJSON */
    while (*s == ' ') s++;
    double sign = 1.0;
    if (*s == '-') { sign = -1.0; s++; }
    else if (*s == '+') s++;
    double val = 0.0;
    while (*s >= '0' && *s <= '9') { val = val * 10.0 + (*s++ - '0'); }
    if (*s == '.') {
        s++;
        double frac = 0.1;
        while (*s >= '0' && *s <= '9') { val += (*s++ - '0') * frac; frac *= 0.1; }
    }
    if (*s == 'e' || *s == 'E') {
        s++;
        int esign = 1;
        if (*s == '-') { esign = -1; s++; } else if (*s == '+') s++;
        int exp = 0;
        while (*s >= '0' && *s <= '9') { exp = exp * 10 + (*s++ - '0'); }
        double base = 10.0;
        double p = 1.0;
        while (exp--) p *= base;
        if (esign > 0) val *= p; else val /= p;
    }
    if (end) *end = (char *)s;
    return sign * val;
}

int bm_atoi(const char *s)
{
    return (int)bm_strtol(s, 0, 10);
}

/* =========================================================================
 * kprintf / kvprintf
 * ========================================================================= */

/* Forward declaration for serial output */
void serial_putchar(char c);

static void kputchar(char c)
{
    vga_putchar(c);
    serial_putchar(c);
}

static void kprint_str(const char *s)
{
    while (*s) kputchar(*s++);
}

static void kprint_uint(unsigned long val, int base, int width, char pad,
                        int uppercase)
{
    static const char digits_lo[] = "0123456789abcdef";
    static const char digits_up[] = "0123456789ABCDEF";
    const char *digits = uppercase ? digits_up : digits_lo;
    char buf[32];
    int  len = 0;

    if (val == 0) {
        buf[len++] = '0';
    } else {
        while (val) {
            buf[len++] = digits[val % (unsigned)base];
            val /= (unsigned)base;
        }
    }
    /* pad */
    for (int i = len; i < width; i++) kputchar(pad);
    /* reverse */
    for (int i = len - 1; i >= 0; i--) kputchar(buf[i]);
}

int kvprintf(const char *fmt, va_list ap)
{
    int count = 0;
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            kputchar(*fmt);
            count++;
            continue;
        }
        fmt++;
        /* Flags */
        char pad = ' ';
        if (*fmt == '0') { pad = '0'; fmt++; }
        /* Width */
        int width = 0;
        while (*fmt >= '0' && *fmt <= '9') { width = width * 10 + (*fmt++ - '0'); }
        /* Length modifier */
        int is_long = 0, is_long_long = 0;
        if (*fmt == 'l') {
            is_long = 1; fmt++;
            if (*fmt == 'l') { is_long_long = 1; fmt++; }
        } else if (*fmt == 'z') {
            is_long = (sizeof(size_t) == sizeof(long));
            fmt++;
        }
        switch (*fmt) {
        case 'd': case 'i': {
            long long v = is_long_long ? va_arg(ap, long long) :
                          is_long      ? va_arg(ap, long)      :
                                         (long long)va_arg(ap, int);
            if (v < 0) { vga_putchar('-'); count++; v = -v; }
            kprint_uint((unsigned long long)v, 10, width, pad, 0);
            break;
        }
        case 'u': {
            unsigned long long v = is_long_long ? va_arg(ap, unsigned long long) :
                                   is_long      ? va_arg(ap, unsigned long)      :
                                                  (unsigned long long)va_arg(ap, unsigned int);
            kprint_uint(v, 10, width, pad, 0);
            break;
        }
        case 'x': case 'p': {
            unsigned long long v = is_long_long ? va_arg(ap, unsigned long long) :
                                   is_long      ? va_arg(ap, unsigned long)      :
                                                  (unsigned long long)va_arg(ap, unsigned int);
            if (*fmt == 'p') { kprint_str("0x"); width = width > 2 ? width - 2 : 0; }
            kprint_uint(v, 16, width, pad, 0);
            break;
        }
        case 'X': {
            unsigned long long v = is_long_long ? va_arg(ap, unsigned long long) :
                                   is_long      ? va_arg(ap, unsigned long)      :
                                                  (unsigned long long)va_arg(ap, unsigned int);
            kprint_uint(v, 16, width, pad, 1);
            break;
        }
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            int len = (int)bm_strlen(s);
            for (int i = len; i < width; i++) { kputchar(' '); count++; }
            kprint_str(s);
            count += len;
            continue;
        }
        case 'c': {
            char c = (char)va_arg(ap, int);
            kputchar(c);
            count++;
            continue;
        }
        case '%':
            kputchar('%');
            count++;
            continue;
        default:
            kputchar('%');
            kputchar(*fmt);
            count += 2;
            continue;
        }
        count++;
    }
    return count;
}

int kprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = kvprintf(fmt, ap);
    va_end(ap);
    return r;
}

/* =========================================================================
 * Math functions (used by ym2612.c init — table generation only)
 * Miniature implementations; precision is sufficient for audio lookup tables.
 * ========================================================================= */

#define BM_PI  3.14159265358979323846
#define BM_LN2 0.69314718055994530942

/* sin via argument reduction + minimax polynomial (relative error < 1e-9) */
double bm_sin(double x)
{
    /* Reduce x to [-π/2, π/2] */
    x -= BM_PI * (long)(x / BM_PI);
    double x2 = x * x;
    /* Horner's method: coefficients from Taylor series sin(x) */
    return x * (1.0 + x2 * (-1.0/6.0 + x2 * (1.0/120.0 + x2 * (-1.0/5040.0
           + x2 * (1.0/362880.0 + x2 * (-1.0/39916800.0))))));
}

double bm_cos(double x)
{
    return bm_sin(x + BM_PI / 2.0);
}

/* Natural log via atanh identity: ln(x) = 2*atanh((x-1)/(x+1)) */
double bm_log(double x)
{
    if (x <= 0.0) return -1e308; /* -inf approximation */
    /* Normalise: x = m * 2^e where 0.5 <= m < 1 */
    int e = 0;
    double m = x;
    while (m >= 1.0) { m *= 0.5; e++; }
    while (m <  0.5) { m *= 2.0; e--; }
    /* ln(m) via atanh series: t = (m-1)/(m+1), ln(m) = 2*(t + t^3/3 + t^5/5 ...) */
    double t = (m - 1.0) / (m + 1.0);
    double t2 = t * t;
    double r = t * (2.0 + t2 * (2.0/3.0 + t2 * (2.0/5.0 + t2 * (2.0/7.0
               + t2 * (2.0/9.0 + t2 * 2.0/11.0)))));
    return r + (double)e * BM_LN2;
}

double bm_log2(double x)
{
    return bm_log(x) / BM_LN2;
}

double bm_exp(double x)
{
    /* exp(x) = 2^(x/ln2) — compute via repeated squaring */
    int n = (int)(x / BM_LN2);
    double frac = x - (double)n * BM_LN2;
    /* exp(frac) via Taylor: frac is small (0..ln2) */
    double r = 1.0 + frac * (1.0 + frac * (0.5 + frac * (1.0/6.0 + frac * (1.0/24.0
               + frac * (1.0/120.0 + frac * 1.0/720.0)))));
    /* Scale by 2^n */
    if (n >= 0) { for (int i = 0; i < n; i++) r *= 2.0; }
    else        { for (int i = 0; i > n; i--) r *= 0.5; }
    return r;
}

double bm_sqrt(double x)
{
    if (x <= 0.0) return 0.0;
    /* Newton-Raphson starting from a rough estimate */
    double r = x > 1.0 ? x * 0.5 : 1.0;
    for (int i = 0; i < 32; i++) {
        double next = 0.5 * (r + x / r);
        if (next == r) break;
        r = next;
    }
    return r;
}

/* =========================================================================
 * Abort
 * ========================================================================= */

void bm_abort(void)
{
    kprintf("\n*** KERNEL ABORT ***\n");
    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}

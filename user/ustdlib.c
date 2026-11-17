// DEOS -- kullanici C kutuphanesi: ctype, dizge, malloc/free (sbrk), stdlib, ortam degiskenleri.
// Kayan nokta, locale, sinyal, setjmp yok.
#include "ulib.h"

// ------------------------------------------------------------------ ctype
int isdigit(int c)  { return c >= '0' && c <= '9'; }
int isupper(int c)  { return c >= 'A' && c <= 'Z'; }
int islower(int c)  { return c >= 'a' && c <= 'z'; }
int isalpha(int c)  { return isupper(c) || islower(c); }
int isalnum(int c)  { return isalpha(c) || isdigit(c); }
int isspace(int c)  { return c == ' ' || (c >= '\t' && c <= '\r'); }
int isprint(int c)  { return c >= 32 && c < 127; }
int isxdigit(int c) { return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
int ispunct(int c)  { return isprint(c) && c != ' ' && !isalnum(c); }
int toupper(int c)  { return islower(c) ? c - 32 : c; }
int tolower(int c)  { return isupper(c) ? c + 32 : c; }

// ------------------------------------------------------------------ dizge
char *strcpy(char *d, const char *s) { char *r = d; while ((*d++ = *s++)); return r; }
char *strncpy(char *d, const char *s, size_t n) { size_t i = 0; for (; i < n && s[i]; i++) d[i] = s[i]; for (; i < n; i++) d[i] = 0; return d; }
char *strcat(char *d, const char *s) { strcpy(d + strlen(d), s); return d; }
char *strncat(char *d, const char *s, size_t n) { char *e = d + strlen(d); size_t i = 0; for (; i < n && s[i]; i++) e[i] = s[i]; e[i] = 0; return d; }
size_t strnlen(const char *s, size_t n) { size_t i = 0; while (i < n && s[i]) i++; return i; }
size_t strspn(const char *s, const char *a)  { size_t n = 0; while (s[n] && strchr(a, s[n])) n++; return n; }
size_t strcspn(const char *s, const char *r) { size_t n = 0; while (s[n] && !strchr(r, s[n])) n++; return n; }
int strcasecmp(const char *a, const char *b) { while (*a && tolower((u8)*a) == tolower((u8)*b)) { a++; b++; } return tolower((u8)*a) - tolower((u8)*b); }
char *strstr(const char *h, const char *n) {
    size_t nl = strlen(n);
    if (!nl) return (char *)h;
    for (; *h; h++) if (*h == *n && !strncmp(h, n, nl)) return (char *)h;
    return 0;
}
void *memmove(void *d, const void *s, size_t n) {
    u8 *p = d; const u8 *q = s;
    if (p == q || !n) return d;
    if (p < q) { while (n--) *p++ = *q++; } else { p += n; q += n; while (n--) *--p = *--q; }
    return d;
}
void *memchr(const void *s, int c, size_t n) { const u8 *p = s; for (; n; n--, p++) if (*p == (u8)c) return (void *)p; return 0; }

// ------------------------------------------------------------------ malloc (sbrk tabanli, ilk uygun + birlestirme)
// Her blok: baslik {boyut, bos, magic, sonraki}; bloklar adres sirasinda bagli listede. free ardisik bos
// bloklari birlestirir; en sondaki bos blok heap'in ucundaysa sbrk ile geri verilmez (basit tutuldu).
typedef struct blk { size_t size; struct blk *next; unsigned magic; unsigned free; } blk_t;   // 24 -> 32 bayt (hizali)
#define MAGIC 0xA110C8EDu
#define HDR   ((size_t)((sizeof(blk_t) + 15) & ~15u))
#define ALIGN16(n) (((n) + 15) & ~(size_t)15)
static blk_t *g_head, *g_tail;
static size_t g_total;

static blk_t *grow(size_t need) {                          // need: veri boyutu
    size_t chunk = ALIGN16(need) + HDR;
    if (chunk < 65536) chunk = 65536;
    char *p = (char *)sys_sbrk((i64)chunk);
    if ((i64)p == -1 || !p) return 0;
    blk_t *b = (blk_t *)p;
    b->size = chunk - HDR; b->next = 0; b->magic = MAGIC; b->free = 1;
    g_total += chunk;
    if (g_tail && (char *)g_tail + HDR + g_tail->size == (char *)b) {     // bitisik: sondaki bos bloga ekle
        if (g_tail->free) { g_tail->size += chunk; return g_tail; }
    }
    if (g_tail) g_tail->next = b; else g_head = b;
    g_tail = b;
    return b;
}

void *malloc(size_t n) {
    if (n == 0) n = 1;
    if (n > 0x40000000u) return 0;
    n = ALIGN16(n);
    for (int pass = 0; pass < 2; pass++) {
        for (blk_t *b = g_head; b; b = b->next) {
            if (!b->free || b->size < n) continue;
            if (b->size >= n + HDR + 32) {                    // bol
                blk_t *r = (blk_t *)((char *)b + HDR + n);
                r->size = b->size - n - HDR; r->next = b->next; r->magic = MAGIC; r->free = 1;
                b->size = n; b->next = r;
                if (g_tail == b) g_tail = r;
            }
            b->free = 0;
            return (char *)b + HDR;
        }
        if (pass == 0 && !grow(n)) return 0;
    }
    return 0;
}

void free(void *p) {
    if (!p) return;
    blk_t *b = (blk_t *)((char *)p - HDR);
    if (b->magic != MAGIC || b->free) {                       // gecersiz ya da cift free: sessizce yutma, durdur
        static const char m[] = "free(): gecersiz gosterici ya da cift free -- durduruluyor\n";
        sys_write(2, m, sizeof m - 1);
        sys_exit(-1);
    }
    b->free = 1;
    for (blk_t *c = g_head; c; c = c->next) {                 // birlestir (ardisik bos bloklar)
        while (c->free && c->next && c->next->free && (char *)c + HDR + c->size == (char *)c->next) {
            blk_t *nx = c->next;
            c->size += HDR + nx->size; c->next = nx->next;
            if (g_tail == nx) g_tail = c;
        }
    }
}

void *calloc(size_t a, size_t b) {
    if (a && b > 0x40000000u / a) return 0;
    void *p = malloc(a * b);
    if (p) memset(p, 0, a * b);
    return p;
}

void *realloc(void *p, size_t n) {
    if (!p) return malloc(n);
    if (n == 0) { free(p); return 0; }
    blk_t *b = (blk_t *)((char *)p - HDR);
    if (b->magic != MAGIC || b->free) return 0;
    if (b->size >= n) return p;
    if (b->next && b->next->free && (char *)b + HDR + b->size == (char *)b->next && b->size + HDR + b->next->size >= n) {
        blk_t *nx = b->next;                                  // sagdaki bos blokla genislet
        b->size += HDR + nx->size; b->next = nx->next;
        if (g_tail == nx) g_tail = b;
        if (b->size >= ALIGN16(n) + HDR + 32) {
            blk_t *r = (blk_t *)((char *)b + HDR + ALIGN16(n));
            r->size = b->size - ALIGN16(n) - HDR; r->next = b->next; r->magic = MAGIC; r->free = 1;
            b->size = ALIGN16(n); b->next = r;
            if (g_tail == b) g_tail = r;
        }
        return p;
    }
    void *q = malloc(n);
    if (!q) return 0;
    memcpy(q, p, b->size);
    free(p);
    return q;
}

size_t heap_used(void) {
    size_t u = 0;
    for (blk_t *b = g_head; b; b = b->next) if (!b->free) u += HDR + b->size;
    return u;
}

char *strdup(const char *s) { size_t n = strlen(s) + 1; char *p = malloc(n); if (p) memcpy(p, s, n); return p; }

// ------------------------------------------------------------------ stdlib
unsigned long strtoul(const char *s, char **end, int base) {
    while (isspace((u8)*s)) s++;
    int neg = 0;
    if (*s == '-') { neg = 1; s++; } else if (*s == '+') s++;
    if ((base == 0 || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { s += 2; base = 16; }
    else if (base == 0) base = (s[0] == '0') ? 8 : 10;
    unsigned long v = 0; const char *st = s;
    for (;; s++) {
        int d = isdigit((u8)*s) ? *s - '0' : isalpha((u8)*s) ? tolower((u8)*s) - 'a' + 10 : 99;
        if (d >= base) break;
        v = v * (unsigned)base + (unsigned)d;
    }
    if (end) *end = (char *)(s == st ? (st - 0) : s);
    return neg ? (unsigned long)(-(long)v) : v;
}
long strtol(const char *s, char **end, int base) { return (long)strtoul(s, end, base); }
int  atoi(const char *s) { return (int)strtol(s, 0, 10); }
long atol(const char *s) { return strtol(s, 0, 10); }
int  abs(int x)   { return x < 0 ? -x : x; }
long labs(long x) { return x < 0 ? -x : x; }

static void swap_bytes(char *a, char *b, size_t n) { while (n--) { char t = *a; *a++ = *b; *b++ = t; } }
static void sift(char *base, size_t sz, size_t start, size_t end, int (*cmp)(const void *, const void *)) {
    size_t root = start;
    while (root * 2 + 1 < end) {
        size_t child = root * 2 + 1;
        if (child + 1 < end && cmp(base + child * sz, base + (child + 1) * sz) < 0) child++;
        if (cmp(base + root * sz, base + child * sz) < 0) { swap_bytes(base + root * sz, base + child * sz, sz); root = child; } else return;
    }
}
void qsort(void *b, size_t n, size_t sz, int (*cmp)(const void *, const void *)) {     // yigin sirasi: ozyineleme yok, en kotu durum O(n log n)
    char *base = b;
    if (n < 2) return;
    for (size_t s = n / 2; s > 0; s--) sift(base, sz, s - 1, n, cmp);
    for (size_t e = n - 1; e > 0; e--) { swap_bytes(base, base + e * sz, sz); sift(base, sz, 0, e, cmp); }
}
void *bsearch(const void *key, const void *base, size_t n, size_t sz, int (*cmp)(const void *, const void *)) {
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = cmp(key, (const char *)base + mid * sz);
        if (c == 0) return (void *)((const char *)base + mid * sz);
        if (c < 0) hi = mid; else lo = mid + 1;
    }
    return 0;
}

static u64 g_rng;
void srand(unsigned seed) { g_rng = seed ? seed : 1; }
int rand(void) {
    if (!g_rng) { u64 r; if (getrandom(&r, 8) == 8 && r) g_rng = r; else g_rng = 88172645463325252ULL; }
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17;
    return (int)((g_rng >> 33) & 0x7FFFFFFF);
}

__attribute__((noreturn)) void abort(void) { fflush(0); sys_exit(-1); }
__attribute__((noreturn)) void exit(int code) { fflush(0); sys_exit(code); }

// ------------------------------------------------------------------ ortam degiskenleri (cekirdek tarafinda tutulur)
char *getenv(const char *name) {
    static char ring[4][208];
    static int idx;
    char *b = ring[idx++ & 3];
    return sc(SYS_GETENV, (i64)name, (i64)b, sizeof ring[0]) >= 0 ? b : 0;
}
int setenv(const char *name, const char *value, int overwrite) {
    if (!overwrite && sc(SYS_GETENV, (i64)name, 0, 0) >= 0) return 0;
    return sc(SYS_SETENV, (i64)name, (i64)value, 0) == 0 ? 0 : -1;
}
int unsetenv(const char *name) { return sc(SYS_SETENV, (i64)name, 0, 0) == 0 ? 0 : -1; }
int environ_block(char *buf, size_t cap) { return (int)sc(SYS_ENVIRON, (i64)buf, (i64)cap, 0); }

// ------------------------------------------------------------------ ag yardimcilari
int parse_ip(const char *s, u32 *out) {
    u32 parts[4] = {0, 0, 0, 0};
    for (int i = 0; i < 4; i++) {
        u32 v = 0; int n = 0;
        while (*s >= '0' && *s <= '9') { v = v * 10 + (u32)(*s - '0'); if (++n > 3) return -1; s++; }
        if (n == 0 || v > 255) return -1;
        parts[i] = v;
        if (i < 3) { if (*s != '.') return -1; s++; }
    }
    if (*s) return -1;
    *out = IP4(parts[0], parts[1], parts[2], parts[3]);
    return 0;
}
void format_ip(u32 ip, char out[16]) { snprintf(out, 16, "%u.%u.%u.%u", ip & 0xFF, (ip >> 8) & 0xFF, (ip >> 16) & 0xFF, (ip >> 24) & 0xFF); }

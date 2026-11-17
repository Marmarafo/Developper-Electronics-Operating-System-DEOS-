// DEOS -- sahte rastgele sayi uretici (bkz. rand.h).
#include "rand.h"
#include "cpu.h"

static UINT64 g_state = 0x9E3779B97F4A7C15ULL;
static int g_rdrand;

static inline UINT64 tsc(void) { UINT32 lo, hi; __asm__ __volatile__ ("rdtsc" : "=a"(lo), "=d"(hi)); return ((UINT64)hi << 32) | lo; }

static int rdrand64(UINT64 *out) {
    for (int i = 0; i < 8; i++) {
        UINT64 v; unsigned char ok;
        __asm__ __volatile__ ("rdrand %0; setc %1" : "=r"(v), "=qm"(ok) : : "cc");
        if (ok) { *out = v; return 1; }
    }
    return 0;
}

void rand_init(void) {
    UINT32 a, b, c, d;
    __asm__ __volatile__ ("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
    g_rdrand = (c >> 30) & 1;
    g_state ^= tsc();
    UINT64 r;
    if (g_rdrand && rdrand64(&r)) g_state ^= r;
}

UINT64 rand64(void) {
    UINT64 f = irq_save();
    g_state += 0x9E3779B97F4A7C15ULL + (tsc() & 0xFFFF);
    UINT64 z = g_state, r;
    if (g_rdrand && rdrand64(&r)) z ^= r;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    z ^= z >> 31;
    irq_restore(f);
    return z;
}

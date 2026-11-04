// DEOS -- kucuk CPU yardimcilari: kesme (IRQ) kilidi.
// Tek islemcili, preemptive cekirdekte "kritik bolge" = kesmeleri kapatmak.
#ifndef DEOS_CPU_H
#define DEOS_CPU_H
#include "types.h"

static inline UINT64 irq_save(void) {
    UINT64 flags;
    __asm__ __volatile__ ("pushfq; pop %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

static inline void irq_restore(UINT64 flags) {
    if (flags & 0x200) __asm__ __volatile__ ("sti" : : : "memory");
}


static inline UINT64 cpu_rdmsr(UINT32 msr) {
    UINT32 lo, hi;
    __asm__ __volatile__ ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((UINT64)hi << 32) | lo;
}

static inline void cpu_wrmsr(UINT32 msr, UINT64 v) {
    __asm__ __volatile__ ("wrmsr" : : "c"(msr), "a"((UINT32)v), "d"((UINT32)(v >> 32)));
}


// CPUID ile islemci marka adi (48 karakter + NUL); desteklenmiyorsa "bilinmiyor".
static inline void cpu_brand(char *out) {
    UINT32 maxext, b, c, d;
    __asm__ __volatile__ ("cpuid" : "=a"(maxext), "=b"(b), "=c"(c), "=d"(d) : "a"(0x80000000u));
    if (maxext < 0x80000004u) { const char *u = "bilinmiyor"; int i = 0; while (u[i]) { out[i] = u[i]; i++; } out[i] = 0; return; }
    UINT32 r[12];
    for (UINT32 i = 0; i < 3; i++)
        __asm__ __volatile__ ("cpuid" : "=a"(r[i*4]), "=b"(r[i*4+1]), "=c"(r[i*4+2]), "=d"(r[i*4+3]) : "a"(0x80000002u + i));
    char *p = (char*)r;
    int s = 0; while (s < 47 && p[s] == ' ') s++;
    int n = 0; for (; s < 48 && p[s]; s++) out[n++] = p[s];
    out[n] = 0;
}

#endif

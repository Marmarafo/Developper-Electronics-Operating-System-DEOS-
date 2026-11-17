// DEOS -- minimal libc bellek/string fonksiyonlari. Derleyici bazen
// (ornegin buyuk struct kopyalarinda) kendiliginden memcpy/memset cagirir,
// bu yuzden freestanding kernelde bunlarin var olmasi sarttir.
#include "string.h"
#include "io.h"

// rep movsb / stosb: modern CPU'larda (ve QEMU'da) bayt dongusunden cok daha hizli.
// Yon bayragi (DF) hicbir zaman set edilmez: kesme gelirse bozulmasin diye geriye kopyalamada std KULLANMIYORUZ.
void *memcpy(void *dst, const void *src, size_t n) {
    void *ret = dst;
    __asm__ __volatile__ ("rep movsb" : "+D"(dst), "+S"(src), "+c"(n) : : "memory");
    return ret;
}

void *memmove(void *dst, const void *src, size_t n) {
    unsigned char *d = dst; const unsigned char *s = src;
    if (d < s) { memcpy(dst, src, n); }
    else if (d > s) { d += n; s += n; while (n--) *--d = *--s; }
    return dst;
}

void *memset(void *dst, int c, size_t n) {
    void *ret = dst;
    __asm__ __volatile__ ("rep stosb" : "+D"(dst), "+c"(n) : "a"((unsigned char)c) : "memory");
    return ret;
}

int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *x = a, *y = b;
    for (; n; n--, x++, y++) if (*x != *y) return (int)*x - (int)*y;
    return 0;
}

size_t strlen(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
    for (; n; n--, a++, b++) {
        if (*a != *b) return (unsigned char)*a - (unsigned char)*b;
        if (!*a) return 0;
    }
    return 0;
}

static int lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

int strcasecmp(const char *a, const char *b) {
    while (*a && lower((unsigned char)*a) == lower((unsigned char)*b)) { a++; b++; }
    return lower((unsigned char)*a) - lower((unsigned char)*b);
}

char *strchr(const char *s, int c) {
    for (; *s; s++) if (*s == (char)c) return (char*)s;
    return c == 0 ? (char*)s : 0;
}

char *strrchr(const char *s, int c) {
    const char *last = 0;
    for (; *s; s++) if (*s == (char)c) last = s;
    return c == 0 ? (char*)s : (char*)last;
}

int strncasecmp(const char *a, const char *b, size_t n) {
    for (; n; n--, a++, b++) {
        int x = lower((unsigned char)*a), y = lower((unsigned char)*b);
        if (x != y) return x - y;
        if (!x) return 0;
    }
    return 0;
}

char *strncpy(char *dst, const char *src, size_t n) {
    size_t i = 0;
    for (; i < n && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = 0;
    return dst;
}

// ---- Yigin koruyucu (stack canary): -fstack-protector-strong -mstack-protector-guard=global
// Deger entry.S'de, kernel_main'den ONCE (RDRAND/TSC ile) rastgele atanir; asagidaki sabit yalnizca
// yedek degerdir. Dusuk bayt 0'dir: dizge tasmalari (strcpy vb.) canary'yi sessizce yeniden uretemez.
UINTN __stack_chk_guard = 0x5A3C96E1B4D20F00ULL;

extern void kernel_panic_text(const char *title, const char *detail, UINT64 rip) __attribute__((noreturn));

__attribute__((noreturn, no_stack_protector)) void __stack_chk_fail(void) {
    const char *msg = "STACK SMASHING DETECTED - HALT\n";           // konsol calismasa bile seri porta yaz
    for (const char *p = msg; *p; p++) {
        while (!(inb(0x3FD) & 0x20));  // THRE
        outb(0x3F8, *p);
    }
    kernel_panic_text("YIGIN TASMASI (stack smashing) -- canary bozuldu",
                      "Bir islev yerel tamponunu asarak donus adresini ezmeye calisti.",
                      (UINT64)(UINTN)__builtin_return_address(0));
}

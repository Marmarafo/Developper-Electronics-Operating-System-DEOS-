#include <stdarg.h>
#include "ulib.h"

u64 strlen(const char *s) { u64 n = 0; while (s[n]) n++; return n; }
int strcmp(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return (u8)*a - (u8)*b; }
int strncmp(const char *a, const char *b, u64 n) { for (; n; n--, a++, b++) { if (*a != *b) return (u8)*a - (u8)*b; if (!*a) return 0; } return 0; }
char *strchr(const char *s, int c) { for (; *s; s++) if (*s == (char)c) return (char*)s; return c ? 0 : (char*)s; }
char *strrchr(const char *s, int c) { const char *l = 0; for (; *s; s++) if (*s == (char)c) l = s; return (char*)l; }
void *memset(void *d, int c, u64 n) { u8 *p = d; while (n--) *p++ = (u8)c; return d; }
void *memcpy(void *d, const void *s, u64 n) { u8 *p = d; const u8 *q = s; while (n--) *p++ = *q++; return d; }
int memcmp(const void *a, const void *b, u64 n) { const u8 *x = a, *y = b; for (; n; n--, x++, y++) if (*x != *y) return *x - *y; return 0; }

void u_puts(const char *s) { sys_write(1, s, strlen(s)); }

static int fmt_num(char *out, int cap, u64 v, int neg, int base, int width, char pad, int left) {
    char tmp[24]; int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v) { tmp[n++] = "0123456789abcdef"[v % base]; v /= base; }
    int total = n + (neg ? 1 : 0), o = 0;
    if (!left && pad == ' ') for (int i = total; i < width && o < cap; i++) out[o++] = ' ';
    if (neg && o < cap) out[o++] = '-';
    if (!left && pad == '0') for (int i = total; i < width && o < cap; i++) out[o++] = '0';
    while (n && o < cap) out[o++] = tmp[--n];
    if (left) for (int i = total; i < width && o < cap; i++) out[o++] = ' ';
    return o;
}

int u_printf(const char *fmt, ...) {
    char out[512];
    int o = 0;
    va_list ap;
    va_start(ap, fmt);
    while (*fmt && o < 500) {
        char c = *fmt++;
        if (c != '%') { out[o++] = c; continue; }
        int left = 0, width = 0, lng = 0; char pad = ' ';
        for (;;) { if (*fmt == '-') { left = 1; fmt++; } else if (*fmt == '0') { pad = '0'; fmt++; } else break; }
        while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        while (*fmt == 'l') { lng++; fmt++; }
        char sp = *fmt++;
        if (!sp) break;
        if (sp == 'd' || sp == 'i') {
            i64 v = lng ? va_arg(ap, i64) : (i64)va_arg(ap, int);
            o += fmt_num(out + o, 511 - o, v < 0 ? (u64)(-v) : (u64)v, v < 0, 10, width, pad, left);
        } else if (sp == 'u') {
            u64 v = lng ? va_arg(ap, u64) : (u64)va_arg(ap, unsigned);
            o += fmt_num(out + o, 511 - o, v, 0, 10, width, pad, left);
        } else if (sp == 'x') {
            u64 v = lng ? va_arg(ap, u64) : (u64)va_arg(ap, unsigned);
            o += fmt_num(out + o, 511 - o, v, 0, 16, width, pad, left);
        } else if (sp == 's') {
            const char *s = va_arg(ap, const char*);
            if (!s) s = "(null)";
            int len = (int)strlen(s);
            if (!left) for (int i = len; i < width && o < 510; i++) out[o++] = ' ';
            for (int i = 0; i < len && o < 510; i++) out[o++] = s[i];
            if (left) for (int i = len; i < width && o < 510; i++) out[o++] = ' ';
        } else if (sp == 'c') { out[o++] = (char)va_arg(ap, int); }
        else if (sp == '%') { out[o++] = '%'; }
    }
    va_end(ap);
    sys_write(1, out, (u64)o);
    return o;
}

void u_putdec(i64 v) { u_printf("%lld", v); }
void u_puthex(u64 v) { u_printf("0x%llx", v); }

const char *u_strerror(i64 e) {
    switch (e) {
    case ENOENT_RET: return "dosya veya dizin yok";
    case EIO_RET: return "G/C hatasi";
    case EBADF_RET: return "gecersiz dosya tanimlayici";
    case ECHILD_RET: return "alt surec yok";
    case EFAULT_RET: return "gecersiz adres";
    case EEXIST_RET: return "zaten var";
    case ENOTDIR_RET: return "dizin degil";
    case EISDIR_RET: return "bir dizin";
    case EINVAL_RET: return "gecersiz arguman";
    case EMFILE_RET: return "cok fazla acik dosya";
    case ENOSPC_RET: return "diskte yer yok";
    case ENOTEMPTY_RET: return "dizin bos degil";
    case ENOSYS_RET: return "desteklenmiyor";
    default: return "hata";
    }
}

// ---- Yigin koruyucu (canary): -fstack-protector-strong -mstack-protector-guard=global
// Deger _start'ta cekirdekten (SYS_RANDOM) alinir; dusuk bayt 0. Tasma tespit edilirse surec -2 ile biter
// (cekirdek "hata nedeniyle sonlandirildi" sayar).
u64 __stack_chk_guard = 0x5A3C96E1B4D20F00ULL;

__attribute__((noreturn, no_stack_protector)) void __stack_chk_fail(void) {
    static const char msg[] = "\n*** YIGIN TASMASI tespit edildi (stack smashing) -- surec sonlandiriliyor ***\n";
    sc(SYS_WRITE, 2, (i64)msg, (i64)(sizeof msg - 1));
    sc(SYS_EXIT, -1, 0, 0);
    for (;;) ;
}

__attribute__((section(".text.start"), noreturn, no_stack_protector)) void _start(int argc, char **argv) {
    u64 g;
    if (sc(SYS_RANDOM, (i64)&g, 8, 0) == 8 && (g & ~0xFFULL)) __stack_chk_guard = g & ~0xFFULL;
    exit(main(argc, argv));
}

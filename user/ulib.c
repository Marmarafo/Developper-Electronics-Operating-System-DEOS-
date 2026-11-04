#include "ulib.h"

void u_puts(const char *s) {
    u64 n = 0;
    while (s[n]) n++;
    sys_write(1, s, n);
}

void u_putdec(i64 v) {
    char buf[24];
    int i = 0, neg = v < 0;
    u64 u = neg ? (u64)(-v) : (u64)v;
    if (u == 0) buf[i++] = '0';
    while (u) { buf[i++] = (char)('0' + u % 10); u /= 10; }
    if (neg) buf[i++] = '-';
    char out[24];
    for (int k = 0; k < i; k++) out[k] = buf[i - 1 - k];
    sys_write(1, out, (u64)i);
}

void u_puthex(u64 v) {
    char out[18];
    out[0] = '0'; out[1] = 'x';
    int n = 0;
    char tmp[16];
    if (v == 0) tmp[n++] = '0';
    while (v) { tmp[n++] = "0123456789abcdef"[v & 15]; v >>= 4; }
    for (int k = 0; k < n; k++) out[2 + k] = tmp[n - 1 - k];
    sys_write(1, out, (u64)(2 + n));
}

void *memset(void *d, int c, u64 n) { u8 *p = d; while (n--) *p++ = (u8)c; return d; }
void *memcpy(void *d, const void *s, u64 n) { u8 *p = d; const u8 *q = s; while (n--) *p++ = *q++; return d; }

__attribute__((section(".text.start"), noreturn)) void _start(void) {
    sys_exit(main());
}

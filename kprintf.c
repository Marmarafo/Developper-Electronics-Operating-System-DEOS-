// DEOS -- kernel printf. Konsola (kprintf) veya tampona (ksnprintf) yazar.
#include <stdarg.h>
#include "types.h"
#include "kprintf.h"
#include "console.h"
#include "cpu.h"

typedef void (*sink_fn)(char c, void *ctx);

static void fmt_number(sink_fn sink, void *ctx, unsigned long long v, int neg,
                       unsigned base, int upper, int width, char pad, int left) {
    char tmp[24];
    int n = 0;
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    if (v == 0) tmp[n++] = '0';
    while (v) { tmp[n++] = digits[v % base]; v /= base; }
    int total = n + (neg ? 1 : 0);
    if (pad == ' ' && !left) for (int i = total; i < width; i++) sink(' ', ctx);
    if (neg) sink('-', ctx);
    if (pad == '0' && !left) for (int i = total; i < width; i++) sink('0', ctx);
    while (n) sink(tmp[--n], ctx);
    if (left) for (int i = total; i < width; i++) sink(' ', ctx);
}

static void kvformat(sink_fn sink, void *ctx, const char *fmt, va_list ap) {
    while (*fmt) {
        char ch = *fmt++;
        if (ch != '%') { sink(ch, ctx); continue; }

        int left = 0; char pad = ' ';
        for (;;) {
            if (*fmt == '-')      { left = 1; fmt++; }
            else if (*fmt == '0') { pad = '0'; fmt++; }
            else break;
        }
        int width = 0;
        while (*fmt >= '0' && *fmt <= '9') { width = width * 10 + (*fmt - '0'); fmt++; }
        int prec = -1;                                            // %.Ns : en fazla N karakter
        if (*fmt == '.') { fmt++; prec = 0; while (*fmt >= '0' && *fmt <= '9') { prec = prec * 10 + (*fmt - '0'); fmt++; } }
        int lcount = 0;
        while (*fmt == 'l') { lcount++; fmt++; }

        char spec = *fmt;
        if (!spec) break;
        fmt++;

        switch (spec) {
        case 'd': case 'i': {
            long long v = lcount ? va_arg(ap, long long) : (long long)va_arg(ap, int);
            int neg = v < 0;
            unsigned long long u = neg ? (0ULL - (unsigned long long)v) : (unsigned long long)v;
            fmt_number(sink, ctx, u, neg, 10, 0, width, pad, left);
            break;
        }
        case 'u': {
            unsigned long long v = lcount ? va_arg(ap, unsigned long long) : (unsigned long long)va_arg(ap, unsigned int);
            fmt_number(sink, ctx, v, 0, 10, 0, width, pad, left);
            break;
        }
        case 'x': case 'X': {
            unsigned long long v = lcount ? va_arg(ap, unsigned long long) : (unsigned long long)va_arg(ap, unsigned int);
            fmt_number(sink, ctx, v, 0, 16, spec == 'X', width, pad, left);
            break;
        }
        case 'p': {
            unsigned long long v = (unsigned long long)(UINTN)va_arg(ap, void*);
            sink('0', ctx); sink('x', ctx);
            fmt_number(sink, ctx, v, 0, 16, 0, 16, '0', 0);
            break;
        }
        case 's': {
            const char *s = va_arg(ap, const char*);
            if (!s) s = "(null)";
            int len = 0; while (s[len] && (prec < 0 || len < prec)) len++;
            if (!left) for (int i = len; i < width; i++) sink(' ', ctx);
            for (int i = 0; i < len; i++) sink(s[i], ctx);
            if (left) for (int i = len; i < width; i++) sink(' ', ctx);
            break;
        }
        case 'c': {
            char c = (char)va_arg(ap, int);
            sink(c, ctx);
            break;
        }
        case '%': sink('%', ctx); break;
        default:  sink('%', ctx); sink(spec, ctx); break;
        }
    }
}

static void console_sink(char c, void *ctx) { (void)ctx; console_putc(c); }

void kprintf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    UINT64 f = irq_save();      // baska thread'in ciktisi araya girmesin
    kvformat(console_sink, 0, fmt, ap);
    irq_restore(f);
    va_end(ap);
}

typedef struct { char *buf; size_t cap; size_t len; } buf_ctx_t;

static void buf_sink(char c, void *ctx) {
    buf_ctx_t *b = ctx;
    if (b->len + 1 < b->cap) b->buf[b->len] = c;
    b->len++;
}

int ksnprintf(char *buf, size_t cap, const char *fmt, ...) {
    buf_ctx_t b = { buf, cap, 0 };
    va_list ap;
    va_start(ap, fmt);
    kvformat(buf_sink, &b, fmt, ap);
    va_end(ap);
    if (cap) buf[b.len < cap ? b.len : cap - 1] = 0;
    return (int)b.len;
}

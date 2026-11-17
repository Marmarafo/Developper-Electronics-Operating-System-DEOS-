// DEOS -- kullanici stdio: bufferli FILE (fopen/fread/fwrite/fgets/fseek...), vsnprintf motoru, printf ailesi.
// Kayan nokta (%f) yok. stdout satir tamponlu, stderr tamponsuz; exit() tum akislari bosaltir.
#include "ulib.h"

// ------------------------------------------------------------------ bicimlendirme
int vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap) {
    size_t o = 0;
    #define PUT(ch) do { if (o + 1 < cap) buf[o] = (char)(ch); o++; } while (0)
    for (; *fmt; fmt++) {
        if (*fmt != '%') { PUT(*fmt); continue; }
        fmt++;
        int left = 0, zero = 0, plus = 0, space = 0, alt = 0;
        for (;; fmt++) {
            if (*fmt == '-') left = 1; else if (*fmt == '0') zero = 1; else if (*fmt == '+') plus = 1;
            else if (*fmt == ' ') space = 1; else if (*fmt == '#') alt = 1; else break;
        }
        int width = 0, prec = -1;
        if (*fmt == '*') { width = va_arg(ap, int); if (width < 0) { left = 1; width = -width; } fmt++; }
        else while (isdigit((u8)*fmt)) width = width * 10 + (*fmt++ - '0');
        if (*fmt == '.') {
            fmt++; prec = 0;
            if (*fmt == '*') { prec = va_arg(ap, int); if (prec < 0) prec = -1; fmt++; }
            else while (isdigit((u8)*fmt)) prec = prec * 10 + (*fmt++ - '0');
        }
        int lng = 0;                                          // 0 int, 1 long, 2 long long/size_t, -1 short, -2 char
        for (;; fmt++) {
            if (*fmt == 'l') lng = lng >= 1 ? 2 : 1; else if (*fmt == 'z' || *fmt == 'j' || *fmt == 't') lng = 2;
            else if (*fmt == 'h') lng = lng <= -1 ? -2 : -1; else break;
        }
        char conv = *fmt;
        if (!conv) break;
        char tmp[72]; int tn = 0;
        const char *str = 0; int slen = 0;
        char sign = 0; const char *pfx = ""; int pl = 0;
        if (conv == 'd' || conv == 'i') {
            i64 v = lng >= 1 ? va_arg(ap, i64) : (i64)va_arg(ap, int);
            if (lng == -1) v = (short)v; else if (lng == -2) v = (signed char)v;
            u64 u = v < 0 ? (u64)(-(v + 1)) + 1 : (u64)v;
            if (v < 0) sign = '-'; else if (plus) sign = '+'; else if (space) sign = ' ';
            if (u == 0 && prec != 0) tmp[tn++] = '0';
            while (u) { tmp[tn++] = (char)('0' + u % 10); u /= 10; }
            str = 0;
        } else if (conv == 'u' || conv == 'x' || conv == 'X' || conv == 'o' || conv == 'p') {
            u64 u;
            if (conv == 'p') { u = (u64)(size_t)va_arg(ap, void *); alt = 1; lng = 2; conv = 'x'; }
            else u = lng >= 1 ? va_arg(ap, u64) : (u64)va_arg(ap, unsigned);
            if (lng == -1) u &= 0xFFFF; else if (lng == -2) u &= 0xFF;
            int base = conv == 'o' ? 8 : conv == 'u' ? 10 : 16;
            const char *dg = conv == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
            if (u == 0 && prec != 0) tmp[tn++] = '0';
            u64 t = u;
            while (t) { tmp[tn++] = dg[t % (u64)base]; t /= (u64)base; }
            if (alt && u != 0 && base == 16) { pfx = conv == 'X' ? "0X" : "0x"; pl = 2; }
            else if (alt && base == 8 && tmp[tn - 1] != '0') tmp[tn++] = '0';
        } else if (conv == 'c') {
            tmp[0] = (char)va_arg(ap, int); str = tmp; slen = 1;
        } else if (conv == 's') {
            str = va_arg(ap, const char *);
            if (!str) str = "(null)";
            slen = 0; while (str[slen] && (prec < 0 || slen < prec)) slen++;
        } else if (conv == '%') {
            PUT('%'); continue;
        } else {
            PUT('%'); PUT(conv); continue;                     // bilinmeyen donusum: oldugu gibi yaz
        }
        // sayi: ham rakamlar tmp[] icinde TERS sirada; precision = en az rakam sayisi
        int digits = tn, zpad = 0;
        if (!str) {
            if (prec > digits) zpad = prec - digits;
            else if (zero && !left && prec < 0) {
                int w = width - (sign ? 1 : 0) - pl - digits;
                if (w > 0) zpad = w;
            }
        }
        int body = str ? slen : (sign ? 1 : 0) + pl + zpad + digits;
        int pad = width > body ? width - body : 0;
        if (!left) for (int i = 0; i < pad; i++) PUT(' ');
        if (str) { for (int i = 0; i < slen; i++) PUT(str[i]); }
        else {
            if (sign) PUT(sign);
            for (int i = 0; i < pl; i++) PUT(pfx[i]);
            for (int i = 0; i < zpad; i++) PUT('0');
            for (int i = digits - 1; i >= 0; i--) PUT(tmp[i]);
        }
        if (left) for (int i = 0; i < pad; i++) PUT(' ');
    }
    if (cap) buf[o < cap ? o : cap - 1] = 0;
    #undef PUT
    return (int)o;
}

int snprintf(char *buf, size_t cap, const char *fmt, ...) { va_list ap; va_start(ap, fmt); int r = vsnprintf(buf, cap, fmt, ap); va_end(ap); return r; }
int sprintf(char *buf, const char *fmt, ...) { va_list ap; va_start(ap, fmt); int r = vsnprintf(buf, 0x7FFFFFFF, fmt, ap); va_end(ap); return r; }

// ------------------------------------------------------------------ FILE
#define F_R 1
#define F_W 2
#define F_APPEND 4
#define F_EOF 8
#define F_ERR 16
#define F_LINE 32
#define F_UNBUF 64
#define F_STATIC 128

struct FILE { int fd, flags, mode, bsize, rpos, rlen, wlen, ungot; u8 *buf; struct FILE *next; };   // mode: 0 yok, 1 okuma, 2 yazma

static u8 g_inbuf[BUFSIZ], g_outbuf[BUFSIZ];
static FILE g_in  = { 0, F_R | F_STATIC, 0, BUFSIZ, 0, 0, 0, -1, g_inbuf, 0 };
static FILE g_out = { 1, F_W | F_LINE | F_STATIC, 0, BUFSIZ, 0, 0, 0, -1, g_outbuf, 0 };
static FILE g_err = { 2, F_W | F_UNBUF | F_STATIC, 0, 0, 0, 0, 0, -1, 0, 0 };
FILE *stdin = &g_in, *stdout = &g_out, *stderr = &g_err;
static FILE *g_files;                                      // acik (statik olmayan) akislar

static int do_flush(FILE *f) {
    if (f->mode == 2 && f->wlen > 0) {
        int off = 0;
        while (off < f->wlen) {
            i64 w = sys_write(f->fd, f->buf + off, (u64)(f->wlen - off));
            if (w <= 0) { f->flags |= F_ERR; f->wlen = 0; return EOF; }
            off += (int)w;
        }
        f->wlen = 0;
    }
    return 0;
}

static void to_reading(FILE *f) {
    if (f->mode == 2) do_flush(f);
    f->mode = 1;
}
static void to_writing(FILE *f) {
    if (f->mode == 1 && f->rlen - f->rpos > 0) lseek(f->fd, -(i64)(f->rlen - f->rpos), SEEK_CUR);   // okunmamis veriyi geri sar
    f->rpos = f->rlen = 0; f->ungot = -1;
    f->mode = 2;
}

int fflush(FILE *f) {
    if (f) return do_flush(f);
    int r = 0;
    if (do_flush(&g_out)) r = EOF;
    for (FILE *p = g_files; p; p = p->next) if (do_flush(p)) r = EOF;
    return r;
}

FILE *fopen(const char *path, const char *mode) {
    int flags = 0, fl = 0;
    int plus = strchr(mode, '+') != 0;
    switch (mode[0]) {
    case 'r': flags = plus ? O_RDWR : O_RDONLY; fl = plus ? F_R | F_W : F_R; break;
    case 'w': flags = (plus ? O_RDWR : O_WRONLY) | O_CREAT | O_TRUNC; fl = plus ? F_R | F_W : F_W; break;
    case 'a': flags = (plus ? O_RDWR : O_WRONLY) | O_CREAT | O_APPEND; fl = (plus ? F_R | F_W : F_W) | F_APPEND; break;
    default: return 0;
    }
    i64 fd = open(path, flags);
    if (fd < 0) return 0;
    FILE *f = calloc(1, sizeof *f);
    u8 *b = malloc(BUFSIZ);
    if (!f || !b) { free(f); free(b); close((int)fd); return 0; }
    f->fd = (int)fd; f->flags = fl; f->bsize = BUFSIZ; f->buf = b; f->ungot = -1;
    f->next = g_files; g_files = f;
    return f;
}

int fclose(FILE *f) {
    if (!f) return EOF;
    int r = do_flush(f);
    if (f->flags & F_STATIC) return r;                    // stdin/stdout/stderr kapatilmaz
    if (close(f->fd) < 0) r = EOF;
    for (FILE **pp = &g_files; *pp; pp = &(*pp)->next) if (*pp == f) { *pp = f->next; break; }
    free(f->buf); free(f);
    return r;
}

static int fill(FILE *f) {
    to_reading(f);
    if (f->flags & F_STATIC && f == &g_in) fflush(&g_out);   // etkilesimli: soru satiri okumadan once gorunsun
    i64 n = sys_read(f->fd, f->buf, (u64)f->bsize);
    f->rpos = 0; f->rlen = 0;
    if (n == 0) { f->flags |= F_EOF; return 0; }
    if (n < 0) { f->flags |= F_ERR; return 0; }
    f->rlen = (int)n;
    return 1;
}

int fgetc(FILE *f) {
    if (!(f->flags & F_R)) { f->flags |= F_ERR; return EOF; }
    if (f->ungot >= 0) { int c = f->ungot; f->ungot = -1; return c; }
    if (f->mode != 1 || f->rpos >= f->rlen) { if (!fill(f)) return EOF; }
    return f->buf[f->rpos++];
}

int ungetc(int c, FILE *f) { if (c == EOF || f->ungot >= 0) return EOF; f->ungot = c & 0xFF; f->flags &= ~F_EOF; return c; }

int fputc(int c, FILE *f) {
    if (!(f->flags & F_W)) { f->flags |= F_ERR; return EOF; }
    if (f->flags & F_UNBUF) { char ch = (char)c; return sys_write(f->fd, &ch, 1) == 1 ? (u8)c : (f->flags |= F_ERR, EOF); }
    if (f->mode != 2) to_writing(f);
    f->buf[f->wlen++] = (u8)c;
    if (f->wlen >= f->bsize || ((f->flags & F_LINE) && c == '\n')) { if (do_flush(f)) return EOF; }
    return (u8)c;
}

size_t fread(void *p, size_t sz, size_t n, FILE *f) {
    u8 *d = p; size_t total = sz * n, got = 0;
    if (!total) return 0;
    while (got < total) {
        if (f->ungot >= 0 || f->mode != 1 || f->rpos >= f->rlen) {
            int c = fgetc(f);
            if (c == EOF) break;
            d[got++] = (u8)c;
            continue;
        }
        size_t k = (size_t)(f->rlen - f->rpos);
        if (k > total - got) k = total - got;
        memcpy(d + got, f->buf + f->rpos, k);
        f->rpos += (int)k; got += k;
    }
    return got / sz;
}

size_t fwrite(const void *p, size_t sz, size_t n, FILE *f) {
    const u8 *s = p; size_t total = sz * n;
    for (size_t i = 0; i < total; i++) if (fputc(s[i], f) == EOF) return i / sz;
    return n;
}

char *fgets(char *s, int n, FILE *f) {
    if (n <= 0) return 0;
    int i = 0;
    while (i < n - 1) {
        int c = fgetc(f);
        if (c == EOF) { if (i == 0) return 0; break; }
        s[i++] = (char)c;
        if (c == '\n') break;
    }
    s[i] = 0;
    return s;
}

int fputs(const char *s, FILE *f) { while (*s) if (fputc((u8)*s++, f) == EOF) return EOF; return 0; }

int fseek(FILE *f, long off, int whence) {
    do_flush(f);
    if (f->mode == 1 && whence == SEEK_CUR) off -= (f->rlen - f->rpos) + (f->ungot >= 0 ? 1 : 0);
    f->rpos = f->rlen = 0; f->ungot = -1; f->mode = 0;
    f->flags &= ~F_EOF;
    return lseek(f->fd, off, whence) < 0 ? -1 : 0;
}

long ftell(FILE *f) {
    i64 p = lseek(f->fd, 0, SEEK_CUR);
    if (p < 0) return -1;
    if (f->mode == 1) p -= (f->rlen - f->rpos) + (f->ungot >= 0 ? 1 : 0);
    else if (f->mode == 2) p += f->wlen;
    return (long)p;
}

void rewind(FILE *f) { fseek(f, 0, SEEK_SET); f->flags &= ~F_ERR; }
int  feof(FILE *f)   { return (f->flags & F_EOF) != 0; }
int  ferror(FILE *f) { return (f->flags & F_ERR) != 0; }
void clearerr(FILE *f) { f->flags &= ~(F_EOF | F_ERR); }
int  fileno(FILE *f) { return f->fd; }
int  remove(const char *path) { return unlink(path) < 0 ? -1 : 0; }

int getchar(void) { return fgetc(stdin); }
int putchar(int c) { return fputc(c, stdout); }
int puts(const char *s) { if (fputs(s, stdout) == EOF) return EOF; return fputc('\n', stdout) == EOF ? EOF : 0; }

int vfprintf(FILE *f, const char *fmt, va_list ap) {
    char small[256];
    va_list ap2; va_copy(ap2, ap);
    int n = vsnprintf(small, sizeof small, fmt, ap);
    int r = n;
    if (n < (int)sizeof small) { if (fputs(small, f) == EOF) r = -1; }
    else {
        char *big = malloc((size_t)n + 1);
        if (!big) r = -1;
        else { vsnprintf(big, (size_t)n + 1, fmt, ap2); if (fputs(big, f) == EOF) r = -1; free(big); }
    }
    va_end(ap2);
    return r;
}
int fprintf(FILE *f, const char *fmt, ...) { va_list ap; va_start(ap, fmt); int r = vfprintf(f, fmt, ap); va_end(ap); return r; }
int printf(const char *fmt, ...) { va_list ap; va_start(ap, fmt); int r = vfprintf(stdout, fmt, ap); va_end(ap); return r; }

void perror(const char *msg) { fprintf(stderr, "%s%s%s\n", msg ? msg : "", msg ? ": " : "", "hata"); }

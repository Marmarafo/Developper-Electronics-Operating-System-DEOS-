#include "ulib.h"
static void dest(const char *src, const char *dst, char *out, int cap) {
    ustat_t st;
    int n = 0;
    while (dst[n] && n < cap - 1) { out[n] = dst[n]; n++; }
    out[n] = 0;
    if (stat(dst, &st) == 0 && st.is_dir) {
        const char *b = strrchr(src, '/'); b = b ? b + 1 : src;
        if (n > 0 && out[n - 1] != '/' && n < cap - 1) out[n++] = '/';
        while (*b && n < cap - 1) out[n++] = *b++;
        out[n] = 0;
    }
}
int main(int argc, char **argv) {
    if (argc != 3) { u_puts("kullanim: cp <kaynak> <hedef>\n"); return 1; }
    char to[160];
    dest(argv[1], argv[2], to, sizeof to);
    i64 in = open(argv[1], O_RDONLY);
    if (in < 0) { u_printf("cp: %s: %s\n", argv[1], u_strerror(in)); return 1; }
    i64 out = open(to, O_WRONLY | O_CREAT | O_TRUNC);
    if (out < 0) { u_printf("cp: %s: %s\n", to, u_strerror(out)); close((int)in); return 1; }
    char buf[4096];
    i64 n, total = 0;
    while ((n = sys_read((int)in, buf, sizeof buf)) > 0) {
        i64 w = sys_write((int)out, buf, (u64)n);
        if (w != n) { u_printf("cp: yazma hatasi: %s\n", u_strerror(w < 0 ? w : ENOSPC_RET)); close((int)in); close((int)out); return 1; }
        total += n;
    }
    close((int)in); close((int)out);
    u_printf("%lld bayt kopyalandi\n", total);
    return 0;
}

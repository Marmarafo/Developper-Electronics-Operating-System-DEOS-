#include "ulib.h"
int main(int argc, char **argv) {
    if (argc < 2) { u_puts("kullanim: hexdump <dosya> [en_fazla_bayt]\n"); return 1; }
    i64 limit = 256;
    if (argc > 2) { limit = 0; for (const char *p = argv[2]; *p >= '0' && *p <= '9'; p++) limit = limit * 10 + (*p - '0'); }
    i64 fd = open(argv[1], O_RDONLY);
    if (fd < 0) { u_printf("hexdump: %s: %s\n", argv[1], u_strerror(fd)); return 1; }
    u8 buf[16];
    i64 off = 0, n;
    while (off < limit && (n = sys_read((int)fd, buf, sizeof buf)) > 0) {
        u_printf("%06llx  ", (u64)off);
        for (int i = 0; i < 16; i++) { if (i < n) u_printf("%02x ", buf[i]); else u_puts("   "); if (i == 7) u_puts(" "); }
        u_puts(" |");
        for (int i = 0; i < n; i++) { char c = (buf[i] >= 32 && buf[i] < 127) ? (char)buf[i] : '.'; sys_write(1, &c, 1); }
        u_puts("|\n");
        off += n;
    }
    close((int)fd);
    return 0;
}

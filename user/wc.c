#include "ulib.h"
int main(int argc, char **argv) {
    if (argc < 2) { u_puts("kullanim: wc <dosya>...\n"); return 1; }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        i64 fd = open(argv[i], O_RDONLY);
        if (fd < 0) { u_printf("wc: %s: %s\n", argv[i], u_strerror(fd)); rc = 1; continue; }
        char buf[1024];
        i64 n, bytes = 0, lines = 0, words = 0;
        int inword = 0;
        while ((n = sys_read((int)fd, buf, sizeof buf)) > 0)
            for (i64 k = 0; k < n; k++) {
                bytes++;
                if (buf[k] == '\n') lines++;
                int sp = buf[k] == ' ' || buf[k] == '\n' || buf[k] == '\t' || buf[k] == '\r';
                if (!sp && !inword) words++;
                inword = !sp;
            }
        close((int)fd);
        u_printf("%8lld %8lld %8lld %s\n", lines, words, bytes, argv[i]);
    }
    return rc;
}

#include "ulib.h"
int main(int argc, char **argv) {
    int rc = 0;
    int nfiles = argc < 2 ? 1 : argc - 1;                       // dosya verilmezse stdin (boru hatti)
    for (int i = 1; i <= nfiles; i++) {
        const char *name = argc < 2 ? "" : argv[i];
        i64 fd = argc < 2 ? 0 : open(argv[i], O_RDONLY);
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
        if (argc >= 2) close((int)fd);
        u_printf("%8lld %8lld %8lld %s\n", lines, words, bytes, name);
    }
    return rc;
}

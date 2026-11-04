#include "ulib.h"
int main(int argc, char **argv) {
    if (argc < 2) { u_puts("kullanim: cat <dosya>...\n"); return 1; }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        i64 fd = open(argv[i], O_RDONLY);
        if (fd < 0) { u_printf("cat: %s: %s\n", argv[i], u_strerror(fd)); rc = 1; continue; }
        char buf[1024];
        i64 n;
        while ((n = sys_read((int)fd, buf, sizeof buf)) > 0) sys_write(1, buf, (u64)n);
        close((int)fd);
    }
    return rc;
}

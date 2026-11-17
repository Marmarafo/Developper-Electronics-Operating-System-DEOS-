// cat [dosya...]: dosyalari (ya da hic verilmezse stdin'i, yani boru hattini) stdout'a yazar.
#include "ulib.h"
static int dump(int fd) {
    char buf[1024];
    i64 n;
    while ((n = sys_read(fd, buf, sizeof buf)) > 0) if (sys_write(1, buf, (u64)n) < 0) return 1;
    return 0;
}
int main(int argc, char **argv) {
    if (argc < 2) return dump(0);
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        i64 fd = open(argv[i], O_RDONLY);
        if (fd < 0) { u_printf("cat: %s: %s\n", argv[i], u_strerror(fd)); rc = 1; continue; }
        if (dump((int)fd)) rc = 1;
        close((int)fd);
    }
    return rc;
}

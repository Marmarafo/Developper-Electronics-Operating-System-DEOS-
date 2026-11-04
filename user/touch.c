#include "ulib.h"
int main(int argc, char **argv) {
    int rc = 0;
    if (argc < 2) { u_puts("kullanim: touch <dosya>...\n"); return 1; }
    for (int i = 1; i < argc; i++) {
        i64 fd = open(argv[i], O_WRONLY | O_CREAT);
        if (fd < 0) { u_printf("touch: %s: %s\n", argv[i], u_strerror(fd)); rc = 1; } else close((int)fd);
    }
    return rc;
}

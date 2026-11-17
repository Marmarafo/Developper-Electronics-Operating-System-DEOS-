#include "ulib.h"
int main(int argc, char **argv) {
    int rc = 0;
    if (argc < 2) { u_puts("kullanim: mkdir <dizin>...\n"); return 1; }
    for (int i = 1; i < argc; i++) { i64 r = mkdir(argv[i]); if (r < 0) { u_printf("mkdir: %s: %s\n", argv[i], u_strerror(r)); rc = 1; } }
    return rc;
}

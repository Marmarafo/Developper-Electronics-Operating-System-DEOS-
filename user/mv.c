#include "ulib.h"
int main(int argc, char **argv) {
    if (argc != 3) { u_puts("kullanim: mv <kaynak> <hedef>\n"); return 1; }
    char to[160];
    ustat_t st;
    u64 n = 0;
    while (argv[2][n] && n < 150) { to[n] = argv[2][n]; n++; }
    to[n] = 0;
    if (stat(argv[2], &st) == 0 && st.is_dir) {
        const char *b = strrchr(argv[1], '/'); b = b ? b + 1 : argv[1];
        if (n > 0 && to[n - 1] != '/') to[n++] = '/';
        while (*b && n < 158) to[n++] = *b++;
        to[n] = 0;
    }
    i64 r = rename(argv[1], to);
    if (r < 0) { u_printf("mv: %s\n", u_strerror(r)); return 1; }
    return 0;
}

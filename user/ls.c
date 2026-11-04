#include "ulib.h"
int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : ".";
    ustat_t st;
    i64 r = stat(path, &st);
    if (r < 0) { u_printf("ls: %s: %s\n", path, u_strerror(r)); return 1; }
    if (!st.is_dir) { u_printf("  %-32s %10u bayt\n", path, st.size); return 0; }
    i64 fd = open(path, O_RDONLY | O_DIRECTORY);
    if (fd < 0) { u_printf("ls: %s: %s\n", path, u_strerror(fd)); return 1; }
    udirent_t d;
    int n = 0;
    while (readdir((int)fd, &d) == 1) {
        if (d.is_dir) u_printf("  %-32s <DIZIN>\n", d.name);
        else          u_printf("  %-32s %10u bayt\n", d.name, d.size);
        n++;
    }
    close((int)fd);
    u_printf("  %d girdi\n", n);
    return 0;
}

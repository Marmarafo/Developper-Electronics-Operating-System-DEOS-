// Etkilesimli: klavyeden bir satir okur, tersini yazar. Bos satir = cikis.
#include "ulib.h"

int main(void) {
    u_puts("reverse: bir satir yazin, tersini yazdirayim (bos satir = cikis)\n");
    for (;;) {
        char line[128];
        int n = 0;
        u_puts("> ");
        for (;;) {
            i64 c = sys_getchar();
            if (c < 0) { sys_sleep(10); continue; }
            if (c == '\n') break;
            if (c == '\b') { if (n > 0) { n--; sys_write(1, "\b", 1); } continue; }
            if (c >= 32 && c < 127 && n < 127) { line[n++] = (char)c; char ch = (char)c; sys_write(1, &ch, 1); }
        }
        u_puts("\n");
        if (n == 0) { u_puts("gule gule!\n"); return 0; }
        for (int i = n - 1; i >= 0; i--) { char ch = line[i]; sys_write(1, &ch, 1); }
        u_puts("\n");
    }
}

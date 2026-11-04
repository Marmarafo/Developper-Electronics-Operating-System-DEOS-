#include "ulib.h"

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    int local = 0;
    u_puts("Merhaba! Ben ring 3'te (kullanici modunda) calisan bir programim.\n");
    u_puts("  pid        : "); u_putdec(sys_getpid()); u_puts("\n");
    u_puts("  tick       : "); u_putdec((i64)sys_ticks()); u_puts("\n");
    u_puts("  stack adr. : "); u_puthex((u64)&local); u_puts("  (kullanici bolgesi)\n");
    u_puts("  kod adresi : "); u_puthex((u64)&main); u_puts("\n");
    return 0;
}

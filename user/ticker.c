#include "ulib.h"

int main(void) {
    i64 pid = sys_getpid();
    for (int i = 1; i <= 5; i++) {
        u_puts("[pid "); u_putdec(pid); u_puts("] tick "); u_putdec(i); u_puts("/5\n");
        sys_sleep(200);
    }
    return 0;
}

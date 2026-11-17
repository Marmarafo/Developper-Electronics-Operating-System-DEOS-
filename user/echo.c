#include "ulib.h"
int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) { if (i > 1) u_puts(" "); u_puts(argv[i]); }
    u_puts("\n");
    return 0;
}

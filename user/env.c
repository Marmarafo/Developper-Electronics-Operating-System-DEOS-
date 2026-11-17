// env: ortam degiskenlerini "AD=DEGER" olarak yazar (cocuk surecler ebeveynin ortamini devralir).
#include "ulib.h"
int main(int argc, char **argv) {
    (void)argc; (void)argv;
    char blk[ENV_MAX_USER];
    int n = environ_block(blk, sizeof blk);
    if (n < 0) { fputs("env: ortam okunamadi\n", stderr); return 1; }
    for (char *p = blk; *p; p += strlen(p) + 1) puts(p);
    return 0;
}

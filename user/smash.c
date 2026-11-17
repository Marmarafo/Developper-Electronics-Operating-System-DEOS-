// smash: kasitli yigin tasmasi. Yigin canary'si (-fstack-protector-strong) bunu yakalamali: surec -1 ile sonlandirilir,
// cekirdek calismaya devam eder. (Kernel testi bunu dogrular.)
#include "ulib.h"
__attribute__((noinline)) static void victim(volatile int n) {
    volatile char buf[16];
    volatile char *p = buf;
    for (int i = 0; i < n; i++) p[i] = 0x41;
}
int main(int argc, char **argv) {
    (void)argc; (void)argv;
    puts("smash: 64 bayt yaziliyor (16 baytlik tampona)...");
    fflush(0);
    victim(64);
    puts("smash: HATA -- canary tasmayi yakalamadi");
    return 0;
}

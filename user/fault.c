// Kernel belleğine erismeye calisir: izolasyon calisiyorsa SADECE bu surec olur.
#include "ulib.h"

int main(void) {
    u_puts("fault: once kernel adresini write() ile yazdirmayi deniyorum...\n");
    i64 r = sys_write(1, (const void*)0xFFFF800000000000ULL, 16);
    u_puts("  write(kernel adresi) donus degeri = "); u_putdec(r);
    u_puts(r < 0 ? "  (reddedildi, iyi)\n" : "  (HATA: kabul edildi!)\n");
    u_puts("simdi dogrudan kernel belleginden okuyorum (0x400000)...\n");
    volatile u64 x = *(volatile u64*)0x400000;
    u_puts("BU SATIR GORUNMEMELI: "); u_puthex(x); u_puts("\n");
    return 0;
}

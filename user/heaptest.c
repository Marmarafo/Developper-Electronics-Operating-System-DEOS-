// sbrk ile heap buyutur, desen yazip dogrular. Desen pid'e bagli: iki surec AYNI sanal
// adreslerde farkli veri tutuyorsa ve ikisi de dogru okuyorsa adres uzaylari izoledir.
#include "ulib.h"

static int fill_check(u8 *p, u64 n, u8 seed) {
    for (u64 i = 0; i < n; i++) p[i] = (u8)(i * 31 + seed);
    sys_yield();                                 // diger surec araya girsin
    for (u64 i = 0; i < n; i++) if (p[i] != (u8)(i * 31 + seed)) return 0;
    return 1;
}

int main(void) {
    u8 seed = (u8)sys_getpid();
    u_puts("[heaptest pid "); u_putdec(sys_getpid()); u_puts("] heap basi: ");
    i64 base = sys_sbrk(0);
    u_puthex((u64)base); u_puts("\n");

    u8 *a = (u8*)sys_sbrk(256 * 1024);
    if ((i64)a < 0) { u_puts("  sbrk basarisiz\n"); return 1; }
    if (!fill_check(a, 256 * 1024, seed)) { u_puts("  HATA: 256 KB bozuk\n"); return 1; }

    u8 *b = (u8*)sys_sbrk(3 * 4096 + 17);
    if ((i64)b < 0) { u_puts("  ikinci sbrk basarisiz\n"); return 1; }
    if (!fill_check(b, 3 * 4096 + 17, (u8)(seed + 1))) { u_puts("  HATA: ikinci blok bozuk\n"); return 1; }
    if (!fill_check(a, 256 * 1024, seed)) { u_puts("  HATA: ilk blok sonradan bozuldu\n"); return 1; }

    u_puts("  tamam: 256 KB + 12 KB heap dogrulandi, yeni brk = ");
    u_puthex((u64)sys_sbrk(0)); u_puts("\n");
    return 0;
}

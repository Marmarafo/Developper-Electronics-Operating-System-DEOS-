// Etkilesimli: kasitli kullanici modu hatalari. Her biri sadece bu sureci oldurmeli.
#include "ulib.h"

#pragma GCC diagnostic ignored "-Winfinite-recursion"
static void deep(int n) { volatile char pad[1024]; pad[0] = (char)n; deep(n + 1); pad[1] = pad[0]; }

int main(void) {
    u_puts("badboy -- bir tusa basin:\n");
    u_puts("  n = NULL oku      k = kernel belleginden oku   w = kernel .text'e yaz\n");
    u_puts("  c = cli (ayricalikli)   h = hlt (ayricalikli)   j = kernel'e atla\n");
    u_puts("  s = stack tasir   d = sifira bol   q = cik\n");
    i64 c;
    while ((c = sys_getchar()) < 0) sys_sleep(10);
    switch (c) {
    case 'n': { volatile u64 x = *(volatile u64*)0; (void)x; break; }
    case 'k': { volatile u64 x = *(volatile u64*)0x100000; (void)x; break; }
    case 'w': { *(volatile u8*)0x400000 = 0x90; break; }
    case 'c': __asm__ __volatile__ ("cli"); break;
    case 'h': __asm__ __volatile__ ("hlt"); break;
    case 'j': ((void (*)(void))0x400000)(); break;
    case 's': deep(0); break;
    case 'd': { volatile int a = 1, b = 0; volatile int r = a / b; (void)r; break; }
    default: u_puts("cikiliyor\n"); return 0;
    }
    u_puts("BU SATIR GORUNMEMELI\n");
    return 0;
}

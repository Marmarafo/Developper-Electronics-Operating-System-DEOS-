#ifndef DEOS_RAND_H
#define DEOS_RAND_H
#include "types.h"
// Sahte rastgele sayi uretici (TSC + varsa RDRAND ile tohumlanir, splitmix64). Kriptografik DEGILDIR;
// yigin koruyucu degeri, DHCP/DNS islem kimligi, TCP baslangic sirasi, gecici port secimi icin yeterlidir.
void   rand_init(void);
UINT64 rand64(void);
#endif

// DEOS -- CMOS gercek zamanli saat (RTC) okuyucu.
#include "types.h"
#include "rtc.h"
#include "io.h"
#include "cpu.h"

static UINT8 cmos(UINT8 reg) { outb(0x70, reg); return inb(0x71); }
static int updating(void) { return cmos(0x0A) & 0x80; }
static int bcd(int v) { return (v & 0x0F) + (v >> 4) * 10; }

void rtc_get(rtc_time_t *t) {
    UINT64 f = irq_save();
    int s, m, h, d, mo, y, b;
    int s2, m2, h2, d2, mo2, y2;
    // Guncelleme sirasinda okumamak icin iki okuma ayni cikana kadar tekrarla
    for (int tries = 0; tries < 5; tries++) {
        for (UINT32 i = 0; i < 100000 && updating(); i++) {}
        s = cmos(0x00); m = cmos(0x02); h = cmos(0x04); d = cmos(0x07); mo = cmos(0x08); y = cmos(0x09);
        for (UINT32 i = 0; i < 100000 && updating(); i++) {}
        s2 = cmos(0x00); m2 = cmos(0x02); h2 = cmos(0x04); d2 = cmos(0x07); mo2 = cmos(0x08); y2 = cmos(0x09);
        if (s == s2 && m == m2 && h == h2 && d == d2 && mo == mo2 && y == y2) break;
    }
    b = cmos(0x0B);
    irq_restore(f);

    int pm = h & 0x80;
    h &= 0x7F;
    if (!(b & 0x04)) { s = bcd(s); m = bcd(m); h = bcd(h); d = bcd(d); mo = bcd(mo); y = bcd(y); }
    if (!(b & 0x02)) { if (pm && h < 12) h += 12; if (!pm && h == 12) h = 0; }   // 12 saat modu
    t->year = 2000 + y; t->month = mo; t->day = d; t->hour = h; t->min = m; t->sec = s;
}

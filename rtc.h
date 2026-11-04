#ifndef DEOS_RTC_H
#define DEOS_RTC_H
typedef struct { int year, month, day, hour, min, sec; } rtc_time_t;
void rtc_get(rtc_time_t *t);     // CMOS gercek zamanli saat (QEMU'da -rtc base=localtime ile yerel saat)
#endif

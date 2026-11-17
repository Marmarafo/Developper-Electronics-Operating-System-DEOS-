#ifndef DEOS_KPRINTF_H
#define DEOS_KPRINTF_H
#include "types.h"
#include "string.h"

// Desteklenen bicimler: %d %i %u %x %X %p %s %c %% ; 'l'/'ll' uzunluk,
// '0' dolgu, '-' sola yasla (sadece %s), genislik (ornegin %08llx, %-10s).
void kprintf(const char *fmt, ...);
int  ksnprintf(char *buf, size_t cap, const char *fmt, ...);

#endif

// DEOS -- x86 port G/C (in/out) yardimci fonksiyonlari.
// PIC, ileride klavye/disk gibi donanimlarla konusmak icin kullanilir.
#ifndef DEOS_IO_H
#define DEOS_IO_H
#include "types.h"

static inline void outb(UINT16 port, UINT8 val) {
    __asm__ __volatile__ ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline UINT8 inb(UINT16 port) {
    UINT8 ret;
    __asm__ __volatile__ ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outw(UINT16 port, UINT16 val) {
    __asm__ __volatile__ ("outw %0, %1" : : "a"(val), "Nd"(port));
}

static inline UINT16 inw(UINT16 port) {
    UINT16 ret;
    __asm__ __volatile__ ("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outl(UINT16 port, UINT32 val) {
    __asm__ __volatile__ ("outl %0, %1" : : "a"(val), "Nd"(port));
}

static inline UINT32 inl(UINT16 port) {
    UINT32 ret;
    __asm__ __volatile__ ("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

#endif

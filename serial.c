// DEOS -- COM1 seri port surucusu (sadece cikis). Hata ayiklama icin.
#include "types.h"
#include "serial.h"
#include "io.h"

#define COM1 0x3F8

void serial_init(void) {
    outb(COM1 + 1, 0x00);  // kesmeleri kapat
    outb(COM1 + 3, 0x80);  // DLAB ac (baud ayari icin)
    outb(COM1 + 0, 0x01);  // bolen = 1 -> 115200 baud
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);  // 8 bit, parity yok, 1 stop bit
    outb(COM1 + 2, 0xC7);  // FIFO ac/temizle
    outb(COM1 + 4, 0x0B);  // RTS/DSR
}

void serial_putc(char c) {
    // Gonderici bosalana kadar bekle (zaman asimli: port yoksa takilmasin)
    for (UINT32 i = 0; i < 100000; i++) {
        if (inb(COM1 + 5) & 0x20) break;
    }
    outb(COM1, (UINT8)c);
}

void serial_puts(const char *s) {
    while (*s) {
        if (*s == '\n') serial_putc('\r');
        serial_putc(*s++);
    }
}

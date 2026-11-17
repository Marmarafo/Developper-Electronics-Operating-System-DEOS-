#ifndef DEOS_SERIAL_H
#define DEOS_SERIAL_H
#include "types.h"

// COM1 (0x3F8) seri port. QEMU'da "-serial stdio" ile terminale baglanir:
// ekran bozuk/siyah olsa bile kernel'in ne yaptigini terminalden gorebiliriz.
void serial_init(void);
void serial_putc(char c);
void serial_puts(const char *s);

#endif

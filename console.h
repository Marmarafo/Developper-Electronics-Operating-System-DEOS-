#ifndef DEOS_CONSOLE_H
#define DEOS_CONSOLE_H
#include "types.h"
#include "bootinfo.h"

// Framebuffer uzerinde metin konsolu. Satir 0 "durum cubugu" icin ayrilmistir
// (scroll etmez); metin satir 1'den baslar.
void   console_init(BootInfo *bi);
void   console_full_clear(UINT32 bg);          // TUM ekrani temizle (durum cubugu dahil), scroll bolgesi = tum ekran
void   console_clear(void);                    // sadece metin alanini temizle
void   console_putc(char c);
void   console_puts(const char *s);
void   console_set_color(UINT32 fg, UINT32 bg);
void   console_put_at(UINT32 col, UINT32 row, const char *s, UINT32 fg, UINT32 bg); // imleci oynatmaz, satir sonunu bg ile doldurur
void   console_cursor(int on);                 // yanip sonen imleci ciz/sil
UINT32 console_cols(void);
UINT32 console_rows(void);


// Konsol ciktisini baska bir hedefe (ornegin masaustundeki terminal penceresine) yonlendirmek icin.
typedef struct {
    void (*putc)(char c);
    void (*set_color)(UINT32 fg, UINT32 bg);
    void (*clear)(void);
    int  (*getchar)(void);        // -1: giris yok
    UINT32 cols, rows;
} console_sink_t;

void console_set_sink(const console_sink_t *sink);   // NULL: dogrudan framebuffer
int  console_getchar(void);                           // sink varsa ondan, yoksa klavyeden; -1 = yok

#endif

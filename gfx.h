#ifndef DEOS_GFX_H
#define DEOS_GFX_H
#include "types.h"
#include "bootinfo.h"

// Cift tamponlu 2D grafik katmani. Tum cizimler ARKA TAMPONA yapilir; gfx_flush bir
// dikdortgeni framebuffer'a kopyalar (okuma yok: framebuffer write-combining, okumasi yavas).
// Renkler 0x00RRGGBB. Tum cizim fonksiyonlari gecerli kirpma (clip) dikdortgenine uyar.
int  gfx_init(BootInfo *bi);                 // 1 = basarili
int  gfx_width(void);
int  gfx_height(void);

void gfx_set_clip(int x, int y, int w, int h);        // ekranla kesistirilir
void gfx_clip_intersect(int x, int y, int w, int h);  // mevcut kirpmayla kesistirir
void gfx_get_clip(int *x, int *y, int *w, int *h);

void gfx_rect(int x, int y, int w, int h, UINT32 color);
void gfx_rect_outline(int x, int y, int w, int h, UINT32 color);
void gfx_rect_alpha(int x, int y, int w, int h, UINT32 color, int alpha);   // alpha 0..255
void gfx_vgradient(int x, int y, int w, int h, UINT32 top, UINT32 bottom);

void gfx_char(int x, int y, char ch, UINT32 fg);                 // arka plan seffaf, 8x16
void gfx_char_bg(int x, int y, char ch, UINT32 fg, UINT32 bg);
void gfx_text(int x, int y, const char *s, UINT32 fg);
int  gfx_text_width(const char *s);
void gfx_text_scaled(int x, int y, const char *s, UINT32 fg, int scale, int alpha);

UINT32 gfx_mix(UINT32 a, UINT32 b, int alpha_b);   // a ile b'yi karistirir (alpha_b: b'nin payi, 0..255)

void gfx_flush(int x, int y, int w, int h);        // arka tampon -> ekran
void gfx_cursor_draw(int x, int y);                // fare okunu DOGRUDAN ekrana cizer
void gfx_cursor_erase(int x, int y);               // imlecin altindaki alani arka tampondan geri yukler
#define GFX_CURSOR_W 12
#define GFX_CURSOR_H 17

#endif

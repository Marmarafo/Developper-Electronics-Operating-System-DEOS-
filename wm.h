#ifndef DEOS_WM_H
#define DEOS_WM_H
#include "types.h"
#include "bootinfo.h"

#define WM_TITLE_H 24

typedef struct window {
    int used;
    int x, y, w, h;                    // DIS dikdortgen (baslik cubugu + cerceve dahil)
    char title[32];
    int minimized, closable;
    int tick_interval;                 // 10 ms'lik tick cinsinden; 0 = periyodik cagri yok
    UINT64 next_tick;
    void *data;
    // Callback'ler (hepsi istege bagli). Koordinatlar istemci alanina goredir.
    void (*paint)(struct window *w, int cx, int cy, int cw, int ch);   // istemci alanini ciz (kirpma ayarli)
    void (*on_key)(struct window *w, char c);
    void (*on_click)(struct window *w, int lx, int ly, int buttons);
    int  (*on_tick)(struct window *w, UINT64 ticks);                   // 1 dondururse pencere yeniden cizilir
    void (*on_close)(struct window *w);
} window_t;

// cw x ch = ISTEMCI alani boyutu. Pencere en uste getirilir ve odaklanir. Yer yoksa NULL.
window_t *wm_create(const char *title, int x, int y, int cw, int ch);
void wm_close(window_t *w);
void wm_activate(window_t *w);          // kucultulmusse geri getir, en uste al, odakla
void wm_invalidate_window(window_t *w); // istemci alanini yeniden ciz
void wm_invalidate_rect(int x, int y, int w, int h);

// Masaustunu baslatir (grafik, fare, terminal penceresi, olay thread'i). 1 = basarili.
int  wm_start(BootInfo *bi);
int  wm_active(void);

#endif

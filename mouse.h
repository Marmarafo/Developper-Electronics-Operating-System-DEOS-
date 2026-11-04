#ifndef DEOS_MOUSE_H
#define DEOS_MOUSE_H
#include "types.h"

typedef struct {
    int dx, dy;        // goreli hareket (dy: ekranda asagi = pozitif)
    UINT8 buttons;     // bit0 sol, bit1 sag, bit2 orta
} mouse_event_t;

// PS/2 fareyi (i8042 aux portu, IRQ12) baslatir. 1 = fare cevap verdi.
int  mouse_init(void);
// Siradaki olayi alir; olay yoksa 0 doner.
int  mouse_poll(mouse_event_t *ev);
void mouse_inject(int dx, int dy, UINT8 buttons);   // USB fare olay gonderir


// Tanilama sayaclari ('mouse' komutu)
typedef struct { int init_ok; UINT32 irqs, bytes, packets, dropped; int last_dx, last_dy; UINT8 last_buttons; UINT8 cfg; } mouse_stats_t;
void mouse_get_stats(mouse_stats_t *s);

#endif

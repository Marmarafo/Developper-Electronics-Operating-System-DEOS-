// DEOS -- PS/2 fare suruculeru. i8042 denetleyicisinin ikinci (aux) portunu acar,
// fareye "akis baslat" der; her hareket/dugme olayi 3 baytlik pakettir (IRQ12).
// Paketler bir halka tampona (ring buffer) konur, pencere yoneticisi mouse_poll ile okur.
#include "types.h"
#include "mouse.h"
#include "irq.h"
#include "pic.h"
#include "io.h"
#include "cpu.h"

#define PS2_DATA 0x60
#define PS2_CMD  0x64
#define QSIZE    128

static mouse_event_t g_q[QSIZE];
static volatile UINT32 g_head, g_tail;
static UINT8 g_pkt[3];
static int   g_idx;
static mouse_stats_t g_stats;

static int wait_write(void) { for (UINT32 i = 0; i < 200000; i++) if (!(inb(PS2_CMD) & 2)) return 1; return 0; }
static int wait_read(void)  { for (UINT32 i = 0; i < 200000; i++) if (inb(PS2_CMD) & 1)  return 1; return 0; }

// Fareye (aux aygita) bir bayt gonderir; ACK (0xFA) bekler.
static int mouse_write(UINT8 b) {
    if (!wait_write()) return 0;
    outb(PS2_CMD, 0xD4);
    if (!wait_write()) return 0;
    outb(PS2_DATA, b);
    if (!wait_read()) return 0;
    return inb(PS2_DATA) == 0xFA;
}

static void push(mouse_event_t e) {
    UINT32 next = (g_head + 1) % QSIZE;
    if (next == g_tail) { g_stats.dropped++; return; }          // doluysa at
    g_q[g_head] = e;
    g_head = next;
}

static void handle_byte(UINT8 b) {
    g_stats.bytes++;
    if (g_idx == 0 && !(b & 0x08)) return;       // ilk baytin bit3'u hep 1'dir: degilse senkron kaybi, atla
    g_pkt[g_idx++] = b;
    if (g_idx < 3) return;
    g_idx = 0;
    UINT8 flags = g_pkt[0];
    if (flags & 0xC0) return;                    // tasma (overflow): paketi at
    mouse_event_t e;
    e.dx = (int)g_pkt[1] - ((flags & 0x10) ? 256 : 0);
    e.dy = -((int)g_pkt[2] - ((flags & 0x20) ? 256 : 0));    // PS/2'de yukari pozitif; ekranda tersi
    e.buttons = flags & 7;
    g_stats.packets++; g_stats.last_dx = e.dx; g_stats.last_dy = e.dy; g_stats.last_buttons = e.buttons;
    push(e);
}

static void mouse_irq(void) {
    g_stats.irqs++;
    UINT8 st = inb(PS2_CMD);
    while ((st & 1) && (st & 0x20)) {            // cikis tamponunda FARE verisi var
        handle_byte(inb(PS2_DATA));
        st = inb(PS2_CMD);
    }
}

static int mouse_init_locked(void) {
    while (inb(PS2_CMD) & 1) inb(PS2_DATA);      // bekleyen baytlari bosalt

    if (!wait_write()) return 0;
    outb(PS2_CMD, 0xA8);                         // aux portunu etkinlestir
    if (!wait_write()) return 0;
    outb(PS2_CMD, 0x20);                         // konfigurasyon baytini oku
    if (!wait_read()) return 0;
    UINT8 cfg = inb(PS2_DATA);
    g_stats.cfg = cfg;
    cfg |= 0x02;                                 // IRQ12 etkin
    cfg &= (UINT8)~0x20;                         // aux saati etkin
    if (!wait_write()) return 0;
    outb(PS2_CMD, 0x60);
    if (!wait_write()) return 0;
    outb(PS2_DATA, cfg);

    int ok = mouse_write(0xF6) && mouse_write(0xF4);     // varsayilanlar, akisi baslat
    g_idx = 0;
    g_stats.init_ok = ok;
    irq_register_handler(12, mouse_irq);
    pic_unmask(2);                               // IRQ12 slave PIC'te: master'daki cascade hatti (IRQ2) de acik olmali
    pic_unmask(12);
    return ok;
}

// i8042 ile konusurken kesmeler KAPALI olmali: aksi halde klavyenin IRQ1 isleyicisi
// denetleyicinin cevap baytlarini (konfigurasyon, ACK) kendisi okuyup calar.
int mouse_init(void) {
    UINT64 f = irq_save();
    int ok = mouse_init_locked();
    irq_restore(f);
    return ok;
}

int mouse_poll(mouse_event_t *ev) {
    if (g_tail == g_head) return 0;
    *ev = g_q[g_tail];
    g_tail = (g_tail + 1) % QSIZE;
    return 1;
}

void mouse_get_stats(mouse_stats_t *s) { *s = g_stats; }

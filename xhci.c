// DEOS -- xHCI (USB 3.x) ana denetleyici suruculeru + cihaz numaralandirma.
//
// Polling tabanli (kesme yok): "usb" kernel thread'i olay halkasini bosaltir, HID raporlarini isler ve
// kok portlardaki takma/cikarma degisikliklerini izler. Tum DMA yapilari PMM'den (birebir eslenmis) ayrilir.
#include "types.h"
#include "usb.h"
#include "pci.h"
#include "pmm.h"
#include "paging.h"
#include "thread.h"
#include "timer.h"
#include "kprintf.h"
#include "string.h"
#include "cpu.h"

typedef struct { UINT32 d0, d1, d2, d3; } trb_t;

#define TRB_NORMAL   1
#define TRB_SETUP    2
#define TRB_DATA     3
#define TRB_STATUS   4
#define TRB_LINK     6
#define TRB_ENABLE_SLOT   9
#define TRB_DISABLE_SLOT  10
#define TRB_ADDRESS_DEV   11
#define TRB_CONFIG_EP     12
#define TRB_EVAL_CTX      13
#define TRB_RESET_EP      14
#define TRB_SET_TR_DEQ    16
#define EV_TRANSFER   32
#define EV_CMD_DONE   33
#define EV_PORT_CHG   34

#define RING_TRBS 256
#define MAX_PORTS_SCAN 32
#define PORTSC_RW   ((1u << 9) | (3u << 14) | (7u << 25))     // yazilirken korunacak bitler (PP, PIC, WCE/WDE/WOE)
#define PORTSC_CHG  (0x7Fu << 17)                              // degisim bitleri (RW1C)

static volatile UINT8 *g_cap, *g_op, *g_rt, *g_db;
static int g_ctxsz, g_max_slots, g_max_ports, g_found;
static UINT64 *g_dcbaa;
static xring_t g_cmd;
static trb_t *g_evt;
static UINT64 g_evt_phys;
static int g_evt_deq, g_evt_cycle = 1;
static usbdev_t g_dev[USB_MAX_DEV];
static UINT8 *g_ctrl_buf;
static UINT64 g_ctrl_buf_phys;
static volatile int g_lock, g_port_changed;
static volatile int g_cmd_done, g_cmd_code, g_cmd_slot;
static UINT32 g_events;
static int g_ready;

static inline UINT32 rd(volatile UINT8 *base, UINT32 off) { return *(volatile UINT32*)(base + off); }
static inline void   wr(volatile UINT8 *base, UINT32 off, UINT32 v) { *(volatile UINT32*)(base + off) = v; }

UINT32 usb_event_count(void) { return g_events; }

// Ayni thread tekrar kilitleyebilir (sinif surucusu, numaralandirma sirasinda blok okumasi yapar).
static int g_lock_owner = -1, g_lock_depth;
void usb_lock(void) {
    int me = thread_current_id();
    for (;;) {
        UINT64 f = irq_save();
        if (!g_lock || g_lock_owner == me) { g_lock = 1; g_lock_owner = me; g_lock_depth++; irq_restore(f); return; }
        irq_restore(f);
        thread_yield();
    }
}
void usb_unlock(void) { if (--g_lock_depth <= 0) { g_lock_depth = 0; g_lock_owner = -1; g_lock = 0; } }

UINT64 usb_dma_alloc(UINT64 pages) {
    UINT64 p = pmm_alloc_pages(pages);
    if (p) memset((void*)(UINTN)p, 0, (size_t)(pages * PAGE_SIZE));
    return p;
}

// ------------------------------------------------------------------ halkalar

static int ring_init(xring_t *r) {
    UINT64 p = usb_dma_alloc(1);
    if (!p) return 0;
    r->trbs = (void*)(UINTN)p; r->phys = p; r->enq = 0; r->cycle = 1;
    trb_t *l = &((trb_t*)r->trbs)[RING_TRBS - 1];                 // son girdi: ilk girdiye donen Link TRB
    l->d0 = (UINT32)p; l->d1 = (UINT32)(p >> 32); l->d2 = 0;
    l->d3 = (TRB_LINK << 10) | (1u << 1);                          // Toggle Cycle
    return 1;
}

static UINT64 ring_push(xring_t *r, UINT32 d0, UINT32 d1, UINT32 d2, UINT32 ctrl) {
    trb_t *t = &((trb_t*)r->trbs)[r->enq];
    t->d0 = d0; t->d1 = d1; t->d2 = d2;
    __asm__ __volatile__ ("" : : : "memory");
    t->d3 = ctrl | (UINT32)r->cycle;
    UINT64 pa = r->phys + (UINT64)r->enq * sizeof(trb_t);
    if (++r->enq == RING_TRBS - 1) {
        trb_t *l = &((trb_t*)r->trbs)[RING_TRBS - 1];
        l->d3 = (l->d3 & ~1u) | (UINT32)r->cycle;                  // Link TRB'yi mevcut cycle ile gecerli yap
        r->enq = 0;
        r->cycle ^= 1;
    }
    return pa;
}

static void doorbell(int slot, int target) { wr(g_db, (UINT32)slot * 4, (UINT32)target); }

// ------------------------------------------------------------------- olaylar

static usbdev_t *dev_by_slot(int slot) {
    for (int i = 0; i < USB_MAX_DEV; i++) if (g_dev[i].used && g_dev[i].slot == slot) return &g_dev[i];
    return 0;
}

static void pump(void) {
    int advanced = 0;
    for (;;) {
        trb_t *e = &g_evt[g_evt_deq];
        if ((e->d3 & 1u) != (UINT32)g_evt_cycle) break;
        int type = (int)((e->d3 >> 10) & 63);
        g_events++;
        if (type == EV_CMD_DONE) {
            g_cmd_code = (int)((e->d2 >> 24) & 0xFF);
            g_cmd_slot = (int)((e->d3 >> 24) & 0xFF);
            g_cmd_done = 1;
        } else if (type == EV_TRANSFER) {
            usbdev_t *d = dev_by_slot((int)((e->d3 >> 24) & 0xFF));
            int dci = (int)((e->d3 >> 16) & 0x1F);
            if (d && dci > 0 && dci < 32) {
                d->ep[dci].code = (int)((e->d2 >> 24) & 0xFF);
                d->ep[dci].residual = e->d2 & 0xFFFFFF;
                d->ep[dci].done = 1;
            }
        } else if (type == EV_PORT_CHG) {
            g_port_changed = 1;
        }
        if (++g_evt_deq == RING_TRBS) { g_evt_deq = 0; g_evt_cycle ^= 1; }
        advanced = 1;
    }
    if (advanced) {
        UINT64 erdp = g_evt_phys + (UINT64)g_evt_deq * sizeof(trb_t);
        wr(g_rt, 0x20 + 0x18, (UINT32)erdp | (1u << 3));            // EHB: olay isleyici mesgul bitini temizle
        wr(g_rt, 0x20 + 0x1C, (UINT32)(erdp >> 32));
    }
}

static int wait_flag(volatile int *flag, UINT32 ms) {
    UINT64 deadline = timer_get_ticks() + ms / 10 + 2;
    for (UINT64 spin = 0;; spin++) {
        pump();
        if (*flag) return 0;
        if ((spin & 0x3FF) == 0x3FF) { if (timer_get_ticks() > deadline) return -1; thread_yield(); }
    }
}

// Komut halkasina komut koy, bekle. xHCI tamamlanma kodunu dondurur (1 = basarili); zaman asimi: -1
static int command(UINT32 d0, UINT32 d1, UINT32 ctrl, int *slot_out) {
    g_cmd_done = 0;
    ring_push(&g_cmd, d0, d1, 0, ctrl);
    doorbell(0, 0);
    if (wait_flag(&g_cmd_done, 3000) != 0) return -1;
    if (slot_out) *slot_out = g_cmd_slot;
    return g_cmd_code;
}

// ----------------------------------------------------------------- aktarimlar

int usb_control(usbdev_t *d, UINT8 reqtype, UINT8 req, UINT16 val, UINT16 idx, void *buf, UINT16 len) {
    xep_t *ep = &d->ep[1];
    int in = (reqtype & 0x80) != 0;
    if (len > 4096) return -1;
    if (len && !in) memcpy(g_ctrl_buf, buf, len);

    UINT32 lo = (UINT32)reqtype | ((UINT32)req << 8) | ((UINT32)val << 16);
    UINT32 hi = (UINT32)idx | ((UINT32)len << 16);
    UINT32 trt = len == 0 ? 0 : (in ? 3 : 2);
    ep->done = 0;
    ring_push(&ep->ring, lo, hi, 8, (TRB_SETUP << 10) | (1u << 6) | (trt << 16));                       // Setup (IDT)
    if (len) ring_push(&ep->ring, (UINT32)g_ctrl_buf_phys, (UINT32)(g_ctrl_buf_phys >> 32), len, (TRB_DATA << 10) | (in ? (1u << 16) : 0));
    ring_push(&ep->ring, 0, 0, 0, (TRB_STATUS << 10) | (1u << 5) | ((len && in) ? 0 : (1u << 16)));     // Status (IOC)
    doorbell(d->slot, 1);
    if (wait_flag(&ep->done, 3000) != 0) return -1;
    if (ep->code != 1 && ep->code != 13) return -ep->code;
    if (len && in) memcpy(buf, g_ctrl_buf, len);
    return 0;
}

int usb_bulk(usbdev_t *d, int dci, UINT64 buf_phys, UINT32 len, UINT32 *actual) {
    xep_t *ep = &d->ep[dci];
    if (!ep->valid || d->removed) return -1;
    ep->done = 0;
    ring_push(&ep->ring, (UINT32)buf_phys, (UINT32)(buf_phys >> 32), len, (TRB_NORMAL << 10) | (1u << 5));
    doorbell(d->slot, dci);
    if (wait_flag(&ep->done, 5000) != 0) return -1;
    if (ep->code != 1 && ep->code != 13) return -ep->code;
    if (actual) *actual = len - ep->residual;
    return 0;
}

int usb_arm_interrupt(usbdev_t *d, int dci, UINT64 buf_phys, UINT32 len) {
    xep_t *ep = &d->ep[dci];
    ep->done = 0;
    ring_push(&ep->ring, (UINT32)buf_phys, (UINT32)(buf_phys >> 32), len, (TRB_NORMAL << 10) | (1u << 5) | (1u << 2));   // IOC + ISP
    doorbell(d->slot, dci);
    return 0;
}

// STALL olmus bir uc noktayi kurtar: Reset Endpoint + Set TR Dequeue + CLEAR_FEATURE(HALT)
int usb_clear_halt(usbdev_t *d, int dci, UINT8 ep_addr) {
    xep_t *ep = &d->ep[dci];
    command(0, 0, (TRB_RESET_EP << 10) | ((UINT32)dci << 16) | ((UINT32)d->slot << 24), 0);
    UINT64 deq = ep->ring.phys + (UINT64)ep->ring.enq * sizeof(trb_t);
    command((UINT32)deq | (UINT32)ep->ring.cycle, (UINT32)(deq >> 32), (TRB_SET_TR_DEQ << 10) | ((UINT32)dci << 16) | ((UINT32)d->slot << 24), 0);
    return usb_control(d, 0x02, 1, 0, ep_addr, 0, 0);
}

// ---------------------------------------------------------- baglam (context) kurma

static UINT32 *slot_ctx(UINT8 *ctx_base, int in_input) { return (UINT32*)(ctx_base + (in_input ? g_ctxsz : 0)); }
static UINT32 *ep_ctx(UINT8 *ctx_base, int dci, int in_input) { return (UINT32*)(ctx_base + (size_t)g_ctxsz * (dci + (in_input ? 1 : 0))); }

static void fill_ep(UINT32 *c, int type, int maxpkt, int interval, UINT64 ring_phys, int avg, int esit) {
    c[0] = (UINT32)interval << 16;
    c[1] = (3u << 1) | ((UINT32)type << 3) | ((UINT32)maxpkt << 16);
    c[2] = (UINT32)ring_phys | 1;                                  // DCS = 1
    c[3] = (UINT32)(ring_phys >> 32);
    c[4] = (UINT32)avg | ((UINT32)esit << 16);
}

int usb_configure(usbdev_t *d, const usb_epcfg_t *eps, int n) {
    memset(d->inctx, 0, PAGE_SIZE);
    UINT32 *icc = (UINT32*)d->inctx;
    int maxdci = 1;
    icc[1] = 1;                                                    // A0: slot baglami
    memcpy(slot_ctx(d->inctx, 1), slot_ctx(d->devctx, 0), (size_t)g_ctxsz);
    for (int i = 0; i < n; i++) {
        xep_t *ep = &d->ep[eps[i].dci];
        if (!ring_init(&ep->ring)) return -1;
        ep->valid = 1; ep->type = eps[i].type; ep->maxpkt = eps[i].maxpkt; ep->dir_in = (eps[i].dci & 1);
        icc[1] |= 1u << eps[i].dci;
        if (eps[i].dci > maxdci) maxdci = eps[i].dci;
        int is_int = eps[i].type == 3 || eps[i].type == 7;
        fill_ep(ep_ctx(d->inctx, eps[i].dci, 1), eps[i].type, eps[i].maxpkt, eps[i].interval, ep->ring.phys,
                is_int ? eps[i].maxpkt : (eps[i].maxpkt > 0 ? eps[i].maxpkt : 512), is_int ? eps[i].maxpkt : 0);
    }
    UINT32 *sc = slot_ctx(d->inctx, 1);
    sc[0] = (sc[0] & ~(31u << 27)) | ((UINT32)maxdci << 27);
    int code = command((UINT32)d->inctx_phys, (UINT32)(d->inctx_phys >> 32), (TRB_CONFIG_EP << 10) | ((UINT32)d->slot << 24), 0);
    return code == 1 ? 0 : -1;
}

// ------------------------------------------------------------ numaralandirma

static int fls_(UINT32 v) { int n = 0; while (v) { n++; v >>= 1; } return n; }

const char *usb_speed_name(int s) {
    switch (s) { case 1: return "Full (12M)"; case 2: return "Low (1.5M)"; case 3: return "High (480M)"; case 4: return "Super (5G)"; case 5: return "Super+ (10G)"; default: return "?"; }
}
const char *usb_kind_name(int k) {
    switch (k) { case USB_KBD: return "klavye"; case USB_MOUSE: return "fare"; case USB_MSC: return "depolama"; case USB_HUB: return "hub (desteklenmiyor)"; case USB_OTHER: return "diger"; default: return "-"; }
}

int usb_device_count(void) { int n = 0; for (int i = 0; i < USB_MAX_DEV; i++) if (g_dev[i].used && !g_dev[i].removed) n++; return n; }
usbdev_t *usb_device(int idx) {
    int n = 0;
    for (int i = 0; i < USB_MAX_DEV; i++) if (g_dev[i].used) { if (n++ == idx) return &g_dev[i]; }
    return 0;
}

static void free_dev(usbdev_t *d) {
    if (d->kind == USB_MSC) usb_msc_detach(d);
    command(0, 0, (TRB_DISABLE_SLOT << 10) | ((UINT32)d->slot << 24), 0);
    g_dcbaa[d->slot] = 0;
    d->removed = 1;
    d->used = 0;                                                   // (DMA sayfalari basit tutuldugu icin geri verilmez)
}

static void parse_and_attach(usbdev_t *d, UINT8 *cfg, int total) {
    int off = 0, iface_ok = 0, iface = -1, cfgval = cfg[5];
    int kind = USB_OTHER, hid_kbd = 0, maxpkt_in = 0, in_dci = 0, out_dci = 0, interval = 0, mscin = 0, mscout = 0;
    int in_max = 0, out_max = 0;
    while (off + 2 <= total && cfg[off] >= 2) {
        UINT8 len = cfg[off], type = cfg[off + 1];
        if (type == 4 && len >= 9) {                               // arayuz
            iface_ok = 0;
            if (cfg[off + 3] == 0 && kind == USB_OTHER) {          // alternatif ayar 0, henuz secim yapilmadi
                UINT8 c = cfg[off + 5], s = cfg[off + 6], p = cfg[off + 7];
                if (c == 3 && s == 1 && (p == 1 || p == 2)) { kind = p == 1 ? USB_KBD : USB_MOUSE; hid_kbd = p == 1; iface = cfg[off + 2]; iface_ok = 1; }
                else if (c == 8 && s == 6 && p == 0x50)       { kind = USB_MSC; iface = cfg[off + 2]; iface_ok = 1; }
                else if (c == 9)                              { kind = USB_HUB; }
            }
        } else if (type == 5 && len >= 7 && iface_ok) {            // uc nokta
            UINT8 addr = cfg[off + 2], attr = cfg[off + 3] & 3;
            int maxp = cfg[off + 4] | (cfg[off + 5] << 8), ival = cfg[off + 6];
            int dci = (addr & 15) * 2 + ((addr & 0x80) ? 1 : 0);
            if (kind == USB_KBD || kind == USB_MOUSE) { if (attr == 3 && (addr & 0x80) && !in_dci) { in_dci = dci; maxpkt_in = maxp & 0x7FF; interval = ival; } }
            else if (kind == USB_MSC && attr == 2) {
                if (addr & 0x80) { if (!mscin) { mscin = 1; in_dci = dci; in_max = maxp & 0x7FF; } }
                else             { if (!mscout) { mscout = 1; out_dci = dci; out_max = maxp & 0x7FF; } }
            }
        }
        off += len;
    }
    d->kind = kind;
    if (kind == USB_HUB || kind == USB_OTHER || iface < 0) return;

    if (usb_control(d, 0x00, 9, (UINT16)cfgval, 0, 0, 0) != 0) { d->kind = USB_OTHER; return; }          // SET_CONFIGURATION

    if (kind == USB_KBD || kind == USB_MOUSE) {
        if (!in_dci) { d->kind = USB_OTHER; return; }
        usb_control(d, 0x21, 0x0B, 0, (UINT16)iface, 0, 0);                                              // SET_PROTOCOL(boot)
        usb_control(d, 0x21, 0x0A, 0, (UINT16)iface, 0, 0);                                              // SET_IDLE(0)
        int ival = (d->speed <= 2) ? fls_((UINT32)(interval ? interval : 8) * 8) - 1 : (interval ? interval - 1 : 3);
        if (d->speed <= 2) { if (ival < 3) ival = 3; if (ival > 10) ival = 10; } else { if (ival < 0) ival = 0; if (ival > 15) ival = 15; }
        usb_epcfg_t c = { in_dci, 7, maxpkt_in ? maxpkt_in : 8, ival };
        if (usb_configure(d, &c, 1) != 0 || !usb_hid_attach(d, hid_kbd, in_dci, maxpkt_in ? maxpkt_in : 8)) d->kind = USB_OTHER;
    } else if (kind == USB_MSC) {
        if (!in_dci || !out_dci) { d->kind = USB_OTHER; return; }
        usb_epcfg_t c[2] = { { out_dci, 2, out_max ? out_max : 512, 0 }, { in_dci, 6, in_max ? in_max : 512, 0 } };
        if (usb_configure(d, c, 2) != 0 || !usb_msc_attach(d, in_dci, out_dci)) d->kind = USB_OTHER;
    }
}

static int enumerate(int port) {
    UINT32 portsc_off = 0x400 + (UINT32)(port - 1) * 0x10;
    UINT32 sc = rd(g_op, portsc_off);
    if (!(sc & 1)) return 0;

    if (!(sc & 2)) {                                               // port etkin degil: sifirla
        wr(g_op, portsc_off, (sc & PORTSC_RW) | (1u << 4));
        UINT64 deadline = timer_get_ticks() + 60;
        while (!(rd(g_op, portsc_off) & (1u << 21)) && timer_get_ticks() < deadline) thread_sleep_ms(10);
        sc = rd(g_op, portsc_off);
        wr(g_op, portsc_off, (sc & PORTSC_RW) | PORTSC_CHG);
        sc = rd(g_op, portsc_off);
        if (!(sc & 2)) return 0;
    }
    thread_sleep_ms(30);                                           // sifirlama sonrasi toparlanma suresi
    sc = rd(g_op, portsc_off);
    int speed = (int)((sc >> 10) & 0xF);

    usbdev_t *d = 0;
    for (int i = 0; i < USB_MAX_DEV; i++) if (!g_dev[i].used) { d = &g_dev[i]; break; }
    if (!d) return 0;
    memset(d, 0, sizeof *d);

    int slot = 0;
    if (command(0, 0, TRB_ENABLE_SLOT << 10, &slot) != 1 || slot <= 0 || slot > g_max_slots) return 0;
    d->used = 1; d->slot = slot; d->port = port; d->speed = speed;
    d->devctx_phys = usb_dma_alloc(1); d->inctx_phys = usb_dma_alloc(1);
    if (!d->devctx_phys || !d->inctx_phys || !ring_init(&d->ep[1].ring)) { d->used = 0; return 0; }
    d->devctx = (UINT8*)(UINTN)d->devctx_phys; d->inctx = (UINT8*)(UINTN)d->inctx_phys;
    d->ep[1].valid = 1; d->ep[1].type = 4;
    g_dcbaa[slot] = d->devctx_phys;

    int maxpkt0 = speed == 2 ? 8 : speed == 1 ? 8 : speed == 3 ? 64 : 512;
    UINT32 *icc = (UINT32*)d->inctx;
    icc[1] = 3;                                                    // A0 + A1
    UINT32 *sctx = slot_ctx(d->inctx, 1);
    sctx[0] = ((UINT32)speed << 20) | (1u << 27);
    sctx[1] = (UINT32)port << 16;
    fill_ep(ep_ctx(d->inctx, 1, 1), 4, maxpkt0, 0, d->ep[1].ring.phys, 8, 0);
    if (command((UINT32)d->inctx_phys, (UINT32)(d->inctx_phys >> 32), (TRB_ADDRESS_DEV << 10) | ((UINT32)slot << 24), 0) != 1) {
        d->used = 0; g_dcbaa[slot] = 0;
        command(0, 0, (TRB_DISABLE_SLOT << 10) | ((UINT32)slot << 24), 0);
        return 0;
    }

    UINT8 desc[64];
    if (speed == 1) {                                              // Full-speed: gercek bMaxPacketSize0'i ogren
        if (usb_control(d, 0x80, 6, 0x0100, 0, desc, 8) == 0 && desc[7] != 0 && desc[7] != maxpkt0) {
            maxpkt0 = desc[7];
            memset(d->inctx, 0, PAGE_SIZE);
            ((UINT32*)d->inctx)[1] = 2;                            // A1
            fill_ep(ep_ctx(d->inctx, 1, 1), 4, maxpkt0, 0, d->ep[1].ring.phys, 8, 0);
            command((UINT32)d->inctx_phys, (UINT32)(d->inctx_phys >> 32), (TRB_EVAL_CTX << 10) | ((UINT32)slot << 24), 0);
        }
    }
    if (usb_control(d, 0x80, 6, 0x0100, 0, desc, 18) != 0) { free_dev(d); return 0; }
    d->dclass = desc[4]; d->dsub = desc[5]; d->dproto = desc[6];
    d->vid = (UINT16)(desc[8] | (desc[9] << 8)); d->pid = (UINT16)(desc[10] | (desc[11] << 8));

    UINT8 cfgbuf[1024];
    if (usb_control(d, 0x80, 6, 0x0200, 0, cfgbuf, 9) != 0) { free_dev(d); return 0; }
    int total = cfgbuf[2] | (cfgbuf[3] << 8);
    if (total > (int)sizeof cfgbuf) total = (int)sizeof cfgbuf;
    if (total < 9 || usb_control(d, 0x80, 6, 0x0200, 0, cfgbuf, (UINT16)total) != 0) { free_dev(d); return 0; }
    ksnprintf(d->name, sizeof d->name, "%04x:%04x", d->vid, d->pid);
    parse_and_attach(d, cfgbuf, total);
    g_found++;
    return 1;
}

static void scan_ports(void) {
    for (int p = 1; p <= g_max_ports && p <= MAX_PORTS_SCAN; p++) {
        UINT32 off = 0x400 + (UINT32)(p - 1) * 0x10;
        UINT32 sc = rd(g_op, off);
        usbdev_t *d = 0;
        for (int i = 0; i < USB_MAX_DEV; i++) if (g_dev[i].used && g_dev[i].port == p) d = &g_dev[i];
        if (sc & PORTSC_CHG) wr(g_op, off, (sc & PORTSC_RW) | (sc & PORTSC_CHG));       // degisim bitlerini temizle
        if ((sc & 1) && !d) { kprintf("[usb] port %d: cihaz algilandi (portsc=0x%x)\n", p, sc); if (!enumerate(p)) kprintf("[usb] port %d: numaralandirma basarisiz\n", p); }
        else if (!(sc & 1) && d) { kprintf("[usb] port %d: cihaz cikarildi (%s)\n", p, d->name); free_dev(d); }
    }
}

static void usb_thread(void *arg) {
    (void)arg;
    UINT64 last_scan = 0;
    for (;;) {
        usb_lock();
        pump();
        UINT64 now = timer_get_ticks();
        for (int i = 0; i < USB_MAX_DEV; i++) {
            usbdev_t *d = &g_dev[i];
            if (d->used && !d->removed && (d->kind == USB_KBD || d->kind == USB_MOUSE)) usb_hid_poll(d, now);
        }
        if (g_port_changed || now - last_scan > 100) { g_port_changed = 0; last_scan = now; scan_ports(); }
        usb_unlock();
        thread_sleep_ms(8);
    }
}

static int wait_reg(volatile UINT8 *base, UINT32 off, UINT32 mask, int want_set, UINT32 ms) {
    UINT64 deadline = timer_get_ticks() + ms / 10 + 2;
    while (timer_get_ticks() <= deadline) {
        int set = (rd(base, off) & mask) != 0;
        if (set == want_set) return 1;
        thread_sleep_ms(1);
    }
    return 0;
}

int usb_init(void) {
    const pci_dev_t *pd = pci_find_class(0x0C, 0x03, 0x30);            // xHCI
    if (!pd) return -1;
    pci_enable(pd);
    UINT64 bar = pci_bar(pd, 0);
    if (!bar) return -1;
    volatile UINT8 *base = mmio_map(bar, 0x10000);
    if (!base) return -1;

    g_cap = base;
    UINT32 caplen = base[0];
    UINT32 hcs1 = rd(g_cap, 4), hcs2 = rd(g_cap, 8), hcc1 = rd(g_cap, 0x10);
    g_op = base + caplen;
    g_db = base + (rd(g_cap, 0x14) & ~3u);
    g_rt = base + (rd(g_cap, 0x18) & ~0x1Fu);
    g_max_slots = (int)(hcs1 & 0xFF);
    g_max_ports = (int)(hcs1 >> 24);
    g_ctxsz = (hcc1 & 4) ? 64 : 32;
    if (g_max_slots > 64) g_max_slots = 64;

    // BIOS'tan denetleyiciyi devral (USB Legacy Support, genisletilmis yetenek 1)
    UINT32 xecp = (hcc1 >> 16) & 0xFFFF;
    for (int guard = 0; xecp && guard < 32; guard++) {
        UINT32 off = xecp * 4, cap = rd(base, off);
        if ((cap & 0xFF) == 1 && (cap & (1u << 16))) {
            wr(base, off, cap | (1u << 24));
            wait_reg(base, off, 1u << 16, 0, 1000);
        }
        UINT32 next = (cap >> 8) & 0xFF;
        if (!next) break;
        xecp += next;
    }

    // Sifirla
    wr(g_op, 0, rd(g_op, 0) & ~1u);
    wait_reg(g_op, 4, 1u, 1, 300);                                      // HCH: durdu
    wr(g_op, 0, rd(g_op, 0) | 2u);                                      // HCRST
    if (!wait_reg(g_op, 0, 2u, 0, 1000) || !wait_reg(g_op, 4, 1u << 11, 0, 1000)) return -1;

    g_dcbaa = (UINT64*)(UINTN)usb_dma_alloc(1);
    if (!g_dcbaa) return -1;
    UINT32 nscratch = (((hcs2 >> 21) & 0x1F) << 5) | ((hcs2 >> 27) & 0x1F);
    if (nscratch) {
        UINT64 *arr = (UINT64*)(UINTN)usb_dma_alloc(1);
        if (!arr || nscratch > 512) return -1;
        for (UINT32 i = 0; i < nscratch; i++) { arr[i] = usb_dma_alloc(1); if (!arr[i]) return -1; }
        g_dcbaa[0] = (UINT64)(UINTN)arr;
    }
    UINT64 dcbaa = (UINT64)(UINTN)g_dcbaa;
    wr(g_op, 0x30, (UINT32)dcbaa); wr(g_op, 0x34, (UINT32)(dcbaa >> 32));
    wr(g_op, 0x38, (UINT32)g_max_slots);

    if (!ring_init(&g_cmd)) return -1;
    wr(g_op, 0x18, (UINT32)g_cmd.phys | 1); wr(g_op, 0x1C, (UINT32)(g_cmd.phys >> 32));

    g_evt_phys = usb_dma_alloc(1);
    UINT64 erst = usb_dma_alloc(1);
    if (!g_evt_phys || !erst) return -1;
    g_evt = (trb_t*)(UINTN)g_evt_phys;
    UINT64 *e = (UINT64*)(UINTN)erst;
    e[0] = g_evt_phys; e[1] = RING_TRBS;
    wr(g_rt, 0x20 + 0x08, 1);                                           // ERSTSZ
    wr(g_rt, 0x20 + 0x18, (UINT32)g_evt_phys); wr(g_rt, 0x20 + 0x1C, (UINT32)(g_evt_phys >> 32));   // ERDP
    wr(g_rt, 0x20 + 0x10, (UINT32)erst); wr(g_rt, 0x20 + 0x14, (UINT32)(erst >> 32));               // ERSTBA
    g_evt_deq = 0; g_evt_cycle = 1;

    g_ctrl_buf_phys = usb_dma_alloc(1);
    if (!g_ctrl_buf_phys) return -1;
    g_ctrl_buf = (UINT8*)(UINTN)g_ctrl_buf_phys;

    wr(g_op, 0, rd(g_op, 0) | 1u);                                      // RS: calistir
    if (!wait_reg(g_op, 4, 1u, 0, 1000)) return -1;
    g_ready = 1;

    for (int p = 1; p <= g_max_ports && p <= MAX_PORTS_SCAN; p++) {      // port gucunu ac
        UINT32 off = 0x400 + (UINT32)(p - 1) * 0x10, sc = rd(g_op, off);
        if (!(sc & (1u << 9))) wr(g_op, off, (sc & PORTSC_RW) | (1u << 9));
    }
    thread_sleep_ms(150);                                               // baglantilarin oturmasi (USB 3 link egitimi)

    usb_lock();
    pump();
    scan_ports();
    usb_unlock();
    thread_create("usb", usb_thread, 0);
    return g_found;
}

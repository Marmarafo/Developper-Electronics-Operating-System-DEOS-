#ifndef DEOS_USB_H
#define DEOS_USB_H
#include "types.h"

// DEOS USB yigini: xHCI (USB 3.x/2.x ana denetleyici) + HID klavye/fare + Mass Storage (BOT/SCSI).
// Hub'lar, EHCI/OHCI/UHCI henuz desteklenmiyor.

#define USB_MAX_DEV 16
enum { USB_NONE, USB_KBD, USB_MOUSE, USB_MSC, USB_HUB, USB_OTHER };

typedef struct { void *trbs; UINT64 phys; int enq, cycle; } xring_t;

typedef struct {
    int valid;
    xring_t ring;
    volatile int done;                 // transfer olayi geldi
    volatile int code;                 // xHCI tamamlanma kodu (1 = basarili, 13 = kisa paket)
    volatile UINT32 residual;          // aktarilamayan bayt
    int type, dir_in, maxpkt, interval;
} xep_t;

typedef struct usbdev {
    int used, removed, slot, port, speed;
    UINT8 *devctx, *inctx;
    UINT64 devctx_phys, inctx_phys;
    xep_t ep[32];                      // DCI'ya gore (1 = EP0, 2*n = OUT, 2*n+1 = IN)
    UINT16 vid, pid;
    UINT8 dclass, dsub, dproto;
    int kind;
    char name[48];
    void *drv;                         // sinif surucusunun verisi
    UINT32 reports, errors;            // HID sayaclari
} usbdev_t;

typedef struct { int dci, type, maxpkt, interval; } usb_epcfg_t;     // type: xHCI EP tipi (2 bulk out, 3 int out, 6 bulk in, 7 int in)

int  usb_init(void);                   // bulunan cihaz sayisi; -1: xHCI denetleyici yok
int  usb_device_count(void);
usbdev_t *usb_device(int i);
const char *usb_speed_name(int speed);
const char *usb_kind_name(int kind);
UINT32 usb_event_count(void);          // tanilama

// ---- cekirdek ilkelleri (sinif suruculeri kullanir; usb_lock altinda cagrilmali) ----
int  usb_control(usbdev_t *d, UINT8 reqtype, UINT8 req, UINT16 val, UINT16 idx, void *buf, UINT16 len);   // 0 | <0
int  usb_configure(usbdev_t *d, const usb_epcfg_t *eps, int n);
int  usb_bulk(usbdev_t *d, int dci, UINT64 buf_phys, UINT32 len, UINT32 *actual);                        // 0 | -kod
int  usb_clear_halt(usbdev_t *d, int dci, UINT8 ep_addr);
int  usb_arm_interrupt(usbdev_t *d, int dci, UINT64 buf_phys, UINT32 len);
void usb_lock(void);
void usb_unlock(void);
UINT64 usb_dma_alloc(UINT64 pages);    // sifirlanmis, birebir (identity) eslenmis fiziksel bellek; 0 = yok

// ---- sinif suruculeri ----
int  usb_hid_attach(usbdev_t *d, int is_kbd, int in_dci, int maxpkt);
void usb_hid_poll(usbdev_t *d, UINT64 now);
int  usb_msc_attach(usbdev_t *d, int in_dci, int out_dci);
void usb_msc_detach(usbdev_t *d);

#endif

// DEOS -- USB Mass Storage (Bulk-Only Transport + SCSI): USB bellek/disk. 512 baytlik sektorler.
// Aygit blok katmanina "usb0", "usb1".. olarak kaydedilir, bolumleri taranir, FAT hacimleri /mnt/<ad> altina baglanir.
#include "types.h"
#include "usb.h"
#include "blk.h"
#include "fs.h"
#include "pmm.h"
#include "heap.h"
#include "thread.h"
#include "kprintf.h"
#include "string.h"

#define MAX_SECTORS 128

typedef struct {
    usbdev_t *d;
    int in_dci, out_dci;
    UINT8 *bounce;  UINT64 bounce_phys;          // 64 KB hizali DMA tamponu
    UINT8 *cb;      UINT64 cb_phys;              // CBW (0..30) ve CSW (64..76)
    UINT32 tag;
    int first_blk, nblk;                         // bu aygitin kaydettigi blok aygitlari: [first_blk, first_blk+nblk)
} msc_t;

static int g_msc_count;

static void put32(UINT8 *p, UINT32 v) { p[0] = (UINT8)v; p[1] = (UINT8)(v >> 8); p[2] = (UINT8)(v >> 16); p[3] = (UINT8)(v >> 24); }
static UINT32 get32(const UINT8 *p) { return (UINT32)p[0] | ((UINT32)p[1] << 8) | ((UINT32)p[2] << 16) | ((UINT32)p[3] << 24); }
static UINT32 be32(const UINT8 *p) { return ((UINT32)p[0] << 24) | ((UINT32)p[1] << 16) | ((UINT32)p[2] << 8) | p[3]; }

static void recover(msc_t *m) {
    usb_control(m->d, 0x21, 0xFF, 0, 0, 0, 0);                              // Bulk-Only Mass Storage Reset
    usb_clear_halt(m->d, m->in_dci, (UINT8)(0x80 | (m->in_dci >> 1)));
    usb_clear_halt(m->d, m->out_dci, (UINT8)(m->out_dci >> 1));
}

// Bir SCSI komutu: CBW -> (veri) -> CSW. 0 = iyi, 1 = komut basarisiz, 2 = faz hatasi, -1 = aktarim hatasi.
static int bot(msc_t *m, const UINT8 *cdb, int cdblen, UINT32 dlen, int data_in) {
    usbdev_t *d = m->d;
    if (d->removed) return -1;
    UINT8 *cbw = m->cb;
    memset(cbw, 0, 31);
    put32(cbw, 0x43425355u); put32(cbw + 4, ++m->tag); put32(cbw + 8, dlen);
    cbw[12] = data_in ? 0x80 : 0; cbw[14] = (UINT8)cdblen;
    memcpy(cbw + 15, cdb, (size_t)cdblen);
    UINT32 act = 0;
    if (usb_bulk(d, m->out_dci, m->cb_phys, 31, &act) != 0) { recover(m); return -1; }
    if (dlen) {
        int dci = data_in ? m->in_dci : m->out_dci;
        int r = usb_bulk(d, dci, m->bounce_phys, dlen, &act);
        if (r == -6) usb_clear_halt(d, dci, (UINT8)((data_in ? 0x80 : 0) | (dci >> 1)));       // STALL: CSW'ye gec
        else if (r != 0) { recover(m); return -1; }
    }
    UINT8 *csw = m->cb + 64;
    int r = usb_bulk(d, m->in_dci, m->cb_phys + 64, 13, &act);
    if (r == -6) { usb_clear_halt(d, m->in_dci, (UINT8)(0x80 | (m->in_dci >> 1))); r = usb_bulk(d, m->in_dci, m->cb_phys + 64, 13, &act); }
    if (r != 0 || act != 13) { recover(m); return -1; }
    if (get32(csw) != 0x53425355u || get32(csw + 4) != m->tag) { recover(m); return -1; }
    return csw[12];
}

static int msc_read(blockdev_t *bd, UINT64 lba, UINT32 count, void *buf) {
    msc_t *m = bd->priv;
    int rc = 0;
    UINT8 *out = buf;
    usb_lock();
    while (count && rc == 0) {
        UINT32 c = count > MAX_SECTORS ? MAX_SECTORS : count;
        UINT8 cdb[10] = { 0x28, 0, (UINT8)(lba >> 24), (UINT8)(lba >> 16), (UINT8)(lba >> 8), (UINT8)lba, 0, (UINT8)(c >> 8), (UINT8)c, 0 };
        rc = bot(m, cdb, 10, c * 512, 1);
        if (rc == 0) { memcpy(out, m->bounce, c * 512); out += c * 512; lba += c; count -= c; }
    }
    usb_unlock();
    return rc;
}

static int msc_write(blockdev_t *bd, UINT64 lba, UINT32 count, const void *buf) {
    msc_t *m = bd->priv;
    int rc = 0;
    const UINT8 *in = buf;
    usb_lock();
    while (count && rc == 0) {
        UINT32 c = count > MAX_SECTORS ? MAX_SECTORS : count;
        memcpy(m->bounce, in, c * 512);
        UINT8 cdb[10] = { 0x2A, 0, (UINT8)(lba >> 24), (UINT8)(lba >> 16), (UINT8)(lba >> 8), (UINT8)lba, 0, (UINT8)(c >> 8), (UINT8)c, 0 };
        rc = bot(m, cdb, 10, c * 512, 0);
        if (rc == 0) { in += c * 512; lba += c; count -= c; }
    }
    if (rc == 0) { UINT8 sync[10] = { 0x35, 0, 0, 0, 0, 0, 0, 0, 0, 0 }; bot(m, sync, 10, 0, 0); }   // SYNCHRONIZE CACHE
    usb_unlock();
    return rc;
}

int usb_msc_attach(usbdev_t *d, int in_dci, int out_dci) {
    msc_t *m = kcalloc(1, sizeof *m);
    if (!m) return 0;
    m->d = d; m->in_dci = in_dci; m->out_dci = out_dci;
    UINT64 raw = usb_dma_alloc(32);                                         // 128 KB: icinde 64 KB hizali pencere
    m->cb_phys = usb_dma_alloc(1);
    if (!raw || !m->cb_phys) { kfree(m); return 0; }
    m->bounce_phys = (raw + 0xFFFF) & ~0xFFFFULL;
    m->bounce = (UINT8*)(UINTN)m->bounce_phys;
    m->cb = (UINT8*)(UINTN)m->cb_phys;
    d->drv = m;

    UINT8 inq[6] = { 0x12, 0, 0, 0, 36, 0 };
    if (bot(m, inq, 6, 36, 1) != 0) { d->drv = 0; kfree(m); return 0; }
    char model[44];
    memcpy(model, m->bounce + 8, 8); model[8] = ' '; memcpy(model + 9, m->bounce + 16, 16); model[25] = 0;
    for (int i = 24; i >= 0 && (model[i] == ' ' || model[i] == 0); i--) model[i] = 0;

    int ready = 0;
    for (int tries = 0; tries < 15 && !ready; tries++) {                    // TEST UNIT READY (+ REQUEST SENSE)
        UINT8 tur[6] = { 0, 0, 0, 0, 0, 0 };
        if (bot(m, tur, 6, 0, 0) == 0) { ready = 1; break; }
        UINT8 sense[6] = { 0x03, 0, 0, 0, 18, 0 };
        bot(m, sense, 6, 18, 1);
        thread_sleep_ms(100);
    }
    if (!ready) { d->drv = 0; kfree(m); return 0; }

    UINT8 cap[10] = { 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    if (bot(m, cap, 10, 8, 1) != 0) { d->drv = 0; kfree(m); return 0; }
    UINT32 last = be32(m->bounce), bsz = be32(m->bounce + 4);
    if (bsz != 512) { kprintf("[usb] depolama: %u bayt sektor desteklenmiyor\n", bsz); d->drv = 0; kfree(m); return 0; }

    blockdev_t bd;
    memset(&bd, 0, sizeof bd);
    ksnprintf(bd.name, sizeof bd.name, "usb%d", g_msc_count++);
    memcpy(bd.model, model, sizeof bd.model - 1);
    bd.sectors = (UINT64)last + 1;
    bd.read = msc_read; bd.write = msc_write; bd.priv = m;
    m->first_blk = blk_count();
    blockdev_t *reg = blk_register(&bd);
    if (!reg) { d->drv = 0; kfree(m); return 0; }
    blk_scan_partitions(reg);
    m->nblk = blk_count() - m->first_blk;
    for (int i = m->first_blk; i < m->first_blk + m->nblk; i++) fs_mount_device(blk_get(i));
    kprintf("[usb] depolama %s: %s, %llu MB\n", bd.name, model, bd.sectors / 2048);
    return 1;
}

void usb_msc_detach(usbdev_t *d) {
    msc_t *m = d->drv;
    if (!m) return;
    for (int i = m->first_blk; i < m->first_blk + m->nblk; i++) {
        blockdev_t *b = blk_get(i);
        if (b) { fs_unmount_device(b); blk_mark_removed(b); }
    }
}

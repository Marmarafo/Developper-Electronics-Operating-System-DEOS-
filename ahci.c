// DEOS -- AHCI (SATA) suruculeru.
//
// Denetleyici PCI uzerinden bulunur (sinif 01:06:01), BAR5 (ABAR) MMIO olarak eslenir.
// Her bagli SATA diski icin port "yeniden kurulur": firmware'in komut listesi/FIS alani yerine
// kendi (PMM'den ayrilmis) tamponlarimiz kullanilir. Komutlar polling ile tamamlanir;
// veri aktarimi 64 KB'lik DMA "bounce" tamponu uzerinden yapilir.
#include "types.h"
#include "ahci.h"
#include "pci.h"
#include "pmm.h"
#include "paging.h"
#include "blk.h"
#include "timer.h"
#include "thread.h"
#include "kprintf.h"
#include "string.h"
#include "cpu.h"

#define HBA_CAP   0x00
#define HBA_GHC   0x04
#define HBA_PI    0x0C
#define PORT(n, r) (0x100 + (n) * 0x80 + (r))
#define P_CLB   0x00
#define P_CLBU  0x04
#define P_FB    0x08
#define P_FBU   0x0C
#define P_IS    0x10
#define P_IE    0x14
#define P_CMD   0x18
#define P_TFD   0x20
#define P_SIG   0x24
#define P_SSTS  0x28
#define P_SERR  0x30
#define P_CI    0x38

#define CMD_ST  (1u << 0)
#define CMD_FRE (1u << 4)
#define CMD_FR  (1u << 14)
#define CMD_CR  (1u << 15)
#define TFD_BSY 0x80u
#define TFD_DRQ 0x08u
#define TFD_ERR 0x01u
#define IS_TFES (1u << 30)

#define ATA_IDENTIFY  0xEC
#define ATA_READ_EXT  0x25
#define ATA_WRITE_EXT 0x35
#define ATA_FLUSH_EXT 0xEA

#define BOUNCE_PAGES 16                       // 64 KB = 128 sektor
#define MAX_SECTORS  128
#define GB4 0x100000000ULL

typedef struct { int active; UINT8 *cl, *fis, *ct; UINT64 cl_phys, ct_phys; } port_t;

static volatile UINT8 *g_hba;
static port_t  g_port[32];
static UINT8  *g_bounce;
static UINT64  g_bounce_phys;
static volatile int g_lock;
static int g_s64a;

static inline UINT32 rd(UINT32 off) { return *(volatile UINT32*)(g_hba + off); }
static inline void   wr(UINT32 off, UINT32 v) { *(volatile UINT32*)(g_hba + off) = v; }

static void lock(void) {
    for (;;) {
        UINT64 f = irq_save();
        if (!g_lock) { g_lock = 1; irq_restore(f); return; }
        irq_restore(f);
        thread_yield();
    }
}
static void unlock(void) { g_lock = 0; }

// (reg & mask) == 0 olana dek bekler (ms cinsinden zaman asimi; 10 ms cozunurluk). 1 = tamam.
static int wait_clear(UINT32 reg, UINT32 mask, UINT32 ms) {
    UINT64 deadline = timer_get_ticks() + ms / 10 + 2;
    for (UINT64 spin = 0; spin < 50000000ULL; spin++) {
        if (!(rd(reg) & mask)) return 1;
        if (timer_get_ticks() > deadline) break;
    }
    return 0;
}

static int port_stop(int n) {
    wr(PORT(n, P_CMD), rd(PORT(n, P_CMD)) & ~(CMD_ST | CMD_FRE));
    return wait_clear(PORT(n, P_CMD), CMD_CR | CMD_FR, 500);
}

static void port_start(int n) {
    wait_clear(PORT(n, P_CMD), CMD_CR, 500);
    wr(PORT(n, P_CMD), rd(PORT(n, P_CMD)) | CMD_FRE);
    wr(PORT(n, P_CMD), rd(PORT(n, P_CMD)) | CMD_ST);
}

// Tek bir ATA komutu: FIS hazirla, PRDT doldur, CI'yi kur, tamamlanmasini bekle. 0 = basarili.
static int send(int n, UINT8 cmd, UINT8 device, UINT64 lba, UINT32 count, UINT32 bytes, int write) {
    port_t *p = &g_port[n];
    UINT32 *hdr = (UINT32*)p->cl;
    hdr[0] = 5 | (write ? (1u << 6) : 0) | ((bytes ? 1u : 0u) << 16);     // CFL=5, W, PRDTL
    hdr[1] = 0;
    hdr[2] = (UINT32)p->ct_phys;
    hdr[3] = (UINT32)(p->ct_phys >> 32);

    UINT8 *ct = p->ct;
    memset(ct, 0, 0x90);
    ct[0] = 0x27; ct[1] = 0x80; ct[2] = cmd; ct[7] = device;                // Host->Device register FIS
    ct[4] = (UINT8)lba; ct[5] = (UINT8)(lba >> 8); ct[6] = (UINT8)(lba >> 16);
    ct[8] = (UINT8)(lba >> 24); ct[9] = (UINT8)(lba >> 32); ct[10] = (UINT8)(lba >> 40);
    ct[12] = (UINT8)count; ct[13] = (UINT8)(count >> 8);
    if (bytes) {
        UINT32 *prd = (UINT32*)(ct + 0x80);
        prd[0] = (UINT32)g_bounce_phys; prd[1] = (UINT32)(g_bounce_phys >> 32);
        prd[2] = 0; prd[3] = bytes - 1;
    }

    if (!wait_clear(PORT(n, P_TFD), TFD_BSY | TFD_DRQ, 1000)) return -1;
    wr(PORT(n, P_IS), 0xFFFFFFFFu);
    __asm__ __volatile__ ("" : : : "memory");
    wr(PORT(n, P_CI), 1);

    UINT64 deadline = timer_get_ticks() + 300;                              // 3 sn
    for (UINT64 spin = 0; spin < 200000000ULL; spin++) {
        if (rd(PORT(n, P_IS)) & IS_TFES) { wr(PORT(n, P_IS), 0xFFFFFFFFu); return -2; }
        if (!(rd(PORT(n, P_CI)) & 1)) break;
        if (timer_get_ticks() > deadline) return -3;
    }
    if (rd(PORT(n, P_TFD)) & TFD_ERR) return -4;
    return 0;
}

static int ahci_blk_read(blockdev_t *d, UINT64 lba, UINT32 count, void *buf) {
    int n = (int)(UINTN)d->priv, rc = 0;
    UINT8 *out = buf;
    lock();
    while (count && rc == 0) {
        UINT32 c = count > MAX_SECTORS ? MAX_SECTORS : count;
        rc = send(n, ATA_READ_EXT, 0x40, lba, c, c * 512, 0);
        if (rc == 0) { memcpy(out, g_bounce, c * 512); out += c * 512; lba += c; count -= c; }
    }
    unlock();
    return rc;
}

static int ahci_blk_write(blockdev_t *d, UINT64 lba, UINT32 count, const void *buf) {
    int n = (int)(UINTN)d->priv, rc = 0;
    const UINT8 *in = buf;
    lock();
    while (count && rc == 0) {
        UINT32 c = count > MAX_SECTORS ? MAX_SECTORS : count;
        memcpy(g_bounce, in, c * 512);
        rc = send(n, ATA_WRITE_EXT, 0x40, lba, c, c * 512, 1);
        if (rc == 0) { in += c * 512; lba += c; count -= c; }
    }
    if (rc == 0) rc = send(n, ATA_FLUSH_EXT, 0x40, 0, 0, 0, 0);            // onbellegi diske yaz
    unlock();
    return rc;
}

static int setup_port(int n) {
    UINT64 page = pmm_alloc_page();
    if (!page || (!g_s64a && page >= GB4)) return 0;
    memset((void*)(UINTN)page, 0, PAGE_SIZE);
    port_t *p = &g_port[n];
    p->cl = (UINT8*)(UINTN)page;            p->cl_phys = page;              // 1 KB komut listesi
    p->fis = (UINT8*)(UINTN)(page + 1024);                                  // 256 B alinan FIS
    p->ct = (UINT8*)(UINTN)(page + 2048);   p->ct_phys = page + 2048;       // komut tablosu (128 B hizali)

    if (!port_stop(n)) return 0;
    wr(PORT(n, P_CLB), (UINT32)page);          wr(PORT(n, P_CLBU), (UINT32)(page >> 32));
    wr(PORT(n, P_FB),  (UINT32)(page + 1024)); wr(PORT(n, P_FBU),  (UINT32)((page + 1024) >> 32));
    wr(PORT(n, P_SERR), 0xFFFFFFFFu);
    wr(PORT(n, P_IS),   0xFFFFFFFFu);
    wr(PORT(n, P_IE),   0);                    // kesme yok: polling
    port_start(n);
    p->active = 1;
    return 1;
}

int ahci_init(void) {
    const pci_dev_t *d = pci_find_class(0x01, 0x06, 0x01);
    if (!d) return 0;
    pci_enable(d);
    UINT64 abar = pci_bar(d, 5);
    if (!abar) return 0;
    g_hba = mmio_map(abar, 0x2000);
    if (!g_hba) return 0;

    wr(HBA_GHC, (rd(HBA_GHC) | 0x80000000u) & ~2u);          // AHCI modu acik, genel kesme kapali
    g_s64a = (rd(HBA_CAP) >> 31) & 1;
    UINT32 pi = rd(HBA_PI);

    g_bounce_phys = pmm_alloc_pages(BOUNCE_PAGES);
    if (!g_bounce_phys || (!g_s64a && g_bounce_phys + BOUNCE_PAGES * PAGE_SIZE > GB4)) return 0;
    g_bounce = (UINT8*)(UINTN)g_bounce_phys;

    int found = 0;
    for (int n = 0; n < 32; n++) {
        if (!(pi & (1u << n))) continue;
        UINT32 ssts = rd(PORT(n, P_SSTS));
        if ((ssts & 0xF) != 3 || ((ssts >> 8) & 0xF) != 1) continue;     // bagli ve aktif degil
        if (rd(PORT(n, P_SIG)) != 0x00000101) continue;                   // SATA disk degil (ATAPI vb.)
        if (!setup_port(n)) continue;

        lock();
        int rc = send(n, ATA_IDENTIFY, 0, 0, 0, 512, 0);
        UINT16 id[256];
        if (rc == 0) memcpy(id, g_bounce, 512);
        unlock();
        if (rc != 0) continue;

        blockdev_t dev;
        memset(&dev, 0, sizeof dev);
        ksnprintf(dev.name, sizeof dev.name, "sd%d", found);
        for (int i = 0; i < 20; i++) { dev.model[i * 2] = (char)(id[27 + i] >> 8); dev.model[i * 2 + 1] = (char)(id[27 + i] & 0xFF); }
        int len = 40; while (len > 0 && (dev.model[len - 1] == ' ' || dev.model[len - 1] == 0)) len--;
        dev.model[len] = 0;
        dev.sectors = (id[83] & (1 << 10))
            ? ((UINT64)id[100] | ((UINT64)id[101] << 16) | ((UINT64)id[102] << 32) | ((UINT64)id[103] << 48))
            : ((UINT64)id[60] | ((UINT64)id[61] << 16));
        dev.read = ahci_blk_read; dev.write = ahci_blk_write;
        dev.priv = (void*)(UINTN)n;
        blockdev_t *reg = blk_register(&dev);
        if (reg) { found++; blk_scan_partitions(reg); }
    }
    return found;
}

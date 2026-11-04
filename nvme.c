// DEOS -- NVMe suruculeru: PCI bul, kontrolcuyu resetle, admin + 1 adet I/O kuyrugu
// kur, namespace 1'i blok katmanina "nvme0" olarak kaydet. Polling + global kilit
// (AHCI'deki modelin aynisi). Sinir: 512B sektorlu, <4GB DMA adresi (birebir esleme).
#include "nvme.h"
#include "types.h"
#include "pci.h"
#include "blk.h"
#include "pmm.h"
#include "io.h"
#include "string.h"
#include "kprintf.h"
#include "timer.h"

#define REG_CAP   0x00
#define REG_CC    0x14
#define REG_CSTS  0x1C
#define REG_AQA   0x24
#define REG_ASQ   0x28
#define REG_ACQ   0x30
#define DB_BASE   0x1000

#define AOP_IDENTIFY       0x06
#define AOP_CREATE_SQ      0x01
#define AOP_CREATE_CQ      0x05
#define IOP_FLUSH          0x00
#define IOP_READ           0x02
#define IOP_WRITE          0x01

#define MAX_IO_BYTES   65536

typedef struct { UINT32 d0, d1, d2, d3; UINT64 p1, p2; UINT32 c10, c11, c12, c13, c14, c15; } sqe_t;
typedef struct { UINT32 d0, rsvd, d2, d3; } cqe_t;

typedef struct {
    int used;
    volatile UINT8 *bar;      // MMIO taban (birebir eslenmis)
    UINT64 bar_phys;
    UINT32 dstrd;             // doorbell stride shift: 4 + DSTRD
    UINT32 qdepth;            // girdi sayisi (2..MQES+1 araligindan)
    UINT32 max_to_ms;         // CAP.TO * 500

    UINT64 asq_phys, acq_phys, iosq_phys, iocq_phys, prp_phys;
    sqe_t *asq, *iosq;
    cqe_t *acq, *iocq;
    UINT64 *prp;              // PRP listesi (DMA -> kernel ayni adres)

    UINT32 sq_tail, cq_head;
    UINT8  cq_phase;

    volatile int locked;
} nvme_t;

static nvme_t g_nvme;

static UINT32 rd32(volatile UINT8 *b, UINT32 o) { return *(volatile UINT32 *)(b + o); }
static UINT64 rd64(volatile UINT8 *b, UINT32 o) { return *(volatile UINT64 *)(b + o); }
static void   wr32(volatile UINT8 *b, UINT32 o, UINT32 v) { *(volatile UINT32 *)(b + o) = v; }

static void lock(void)   { while (__sync_lock_test_and_set(&g_nvme.locked, 1)) __asm__ __volatile__("pause"); }
static void unlock(void) { __sync_lock_release(&g_nvme.locked); }

static void db(volatile UINT8 *bar, UINT32 qid, int completion, UINT32 value) {
    UINT32 off = DB_BASE + (2 * qid + (completion ? 1 : 0)) * (4u << g_nvme.dstrd);
    wr32(bar, off, value);
}

static int wait_csts(volatile UINT8 *bar, UINT32 mask, UINT32 want_ms) {
    UINT64 start = timer_get_ticks();
    while (((rd32(bar, REG_CSTS) & mask) ? 1 : 0) == 0) {
        if (timer_get_ticks() - start > (UINT64)want_ms * 100 / 10) return -1;
        __asm__ __volatile__("pause");
    }
    return 0;
}

// Admin komutu gonder; 0 = basarili. (tek thread + kilit disinda cagrilmaz)
static int admin_cmd(sqe_t *cmd) {
    nvme_t *n = &g_nvme;
    UINT16 cid = (UINT16)(n->sq_tail & 0xFFFF);
    cmd->d0 &= ~0xFFFF0000u;                 // CID alanini temizle
    cmd->d0 |= ((UINT32)cid << 16);
    n->asq[n->sq_tail] = *cmd;
    n->sq_tail = (n->sq_tail + 1) % n->qdepth;
    db(n->bar, 0, 0, n->sq_tail);

    for (;;) {
        cqe_t *c = &n->acq[n->cq_head];
        UINT32 d3 = c->d3;
        if ((d3 & 1u) != n->cq_phase) { __asm__ __volatile__("pause"); continue; }
        UINT16 sqid = (UINT16)(c->d2 >> 16);
        UINT32 status = d3 >> 17;            // SCT/SC alanlari; 0 = basarili
        UINT32 head = n->cq_head;
        n->cq_head = (n->cq_head + 1) % n->qdepth;
        if (n->cq_head == 0) n->cq_phase ^= 1;
        db(n->bar, 0, 1, head);              // head yerine eski head yazilir (yeni kapali konum)
        if (sqid != 0) continue;             // bizim admin degilse atla (olmamali)
        return status == 0 ? 0 : -1;
    }
}

// I/O komutu (sqid 1). PRP listesi: nprp>1 ise prp[] sayfa adresleri (bitisik tampon).
static int io_cmd(sqe_t *cmd) {
    nvme_t *n = &g_nvme;
    UINT16 cid = (UINT16)(n->sq_tail & 0xFFFF);
    cmd->d0 &= ~0xFFFF0000u;
    cmd->d0 |= ((UINT32)cid << 16);
    n->iosq[n->sq_tail] = *cmd;
    n->sq_tail = (n->sq_tail + 1) % n->qdepth;
    db(n->bar, 1, 0, n->sq_tail);

    for (;;) {
        cqe_t *c = &n->iocq[n->cq_head];
        UINT32 d3 = c->d3;
        if ((d3 & 1u) != n->cq_phase) { __asm__ __volatile__("pause"); continue; }
        UINT16 sqid = (UINT16)(c->d2 >> 16);
        UINT32 status = d3 >> 17;
        UINT32 head = n->cq_head;
        n->cq_head = (n->cq_head + 1) % n->qdepth;
        if (n->cq_head == 0) n->cq_phase ^= 1;
        db(n->bar, 1, 1, head);
        if (sqid != 1) continue;
        return status == 0 ? 0 : -1;
    }
}

static void prp_setup(UINT64 buf, UINT32 bytes, UINT64 *p1, UINT64 *p2) {
    UINT32 nprp = (bytes + 4095) / 4096;
    if (nprp == 1) { *p1 = buf; *p2 = 0; return; }
    *p1 = buf; *p2 = g_nvme.prp_phys;
    for (UINT32 i = 1; i < nprp; i++) g_nvme.prp[i - 1] = buf + (UINT64)i * 4096;
}

static UINT64 g_ns_size;   // blok sayisi

static int nvme_read(blockdev_t *d, UINT64 lba, UINT32 count, void *buf) {
    (void)d;
    if ((UINT64)count * 512 > MAX_IO_BYTES) return -1;
    lock();
    sqe_t c; memset(&c, 0, sizeof c);
    UINT64 p1, p2;
    prp_setup((UINT64)(UINTN)buf, count * 512, &p1, &p2);
    c.d0 = IOP_READ; c.d1 = 1; c.p1 = p1; c.p2 = p2;
    c.c10 = count - 1; c.c11 = (UINT32)lba; c.c12 = (UINT32)(lba >> 32);
    int r = io_cmd(&c);
    unlock();
    return r;
}

static int nvme_write(blockdev_t *d, UINT64 lba, UINT32 count, const void *buf) {
    (void)d;
    if ((UINT64)count * 512 > MAX_IO_BYTES) return -1;
    lock();
    sqe_t c; memset(&c, 0, sizeof c);
    UINT64 p1, p2;
    prp_setup((UINT64)(UINTN)buf, count * 512, &p1, &p2);
    c.d0 = IOP_WRITE; c.d1 = 1; c.p1 = p1; c.p2 = p2;
    c.c10 = count - 1; c.c11 = (UINT32)lba; c.c12 = (UINT32)(lba >> 32);
    int r = io_cmd(&c);
    if (r == 0) {                          // AHCI davranisina benzer: her yazma sonrasi flush
        sqe_t f; memset(&f, 0, sizeof f);
        f.d0 = IOP_FLUSH; f.d1 = 1;
        r = io_cmd(&f);
    }
    unlock();
    return r;
}

int nvme_init(void) {
    const pci_dev_t *pci = pci_find_class(0x01, 0x08, 0x02);
    if (!pci) return 0;
    pci_enable(pci);

    UINT32 lo = pci_read32(pci, 0x10);
    UINT32 hi = pci_read32(pci, 0x14);
    if ((lo & 0x7) != 0x4) { kprintf("[NVMe] BAR0 MMIO degil\n"); return 0; } // tip=64-bit
    UINT64 bar = ((UINT64)hi << 32) | (lo & ~0xFULL);
    if (bar == 0 || bar >= 0x100000000ULL) { kprintf("[NVMe] BAR >= 4GB (esleme yok)\n"); return 0; }

    nvme_t *n = &g_nvme;
    memset(n, 0, sizeof *n);
    n->bar = (volatile UINT8 *)(UINTN)bar;
    n->bar_phys = bar;
    UINT64 cap = rd64(n->bar, REG_CAP);
    n->dstrd = (UINT32)((cap >> 32) & 0xF);
    UINT32 mqes = (UINT32)(cap & 0xFFFF) + 1;
    n->qdepth = mqes < 32 ? mqes : 32;
    n->max_to_ms = (UINT32)((cap >> 24) & 0xFF) * 500;
    if (n->max_to_ms < 1000) n->max_to_ms = 1000;

    // 1) Kontrolcuyu kapat (aciksa) ve RDY=0 bekle.
    if (rd32(n->bar, REG_CC) & 1) {
        wr32(n->bar, REG_CC, rd32(n->bar, REG_CC) & ~1u);
        UINT64 start = timer_get_ticks();
        while (rd32(n->bar, REG_CSTS) & 1)
            if (timer_get_ticks() - start > (UINT64)n->max_to_ms) { kprintf("[NVMe] reset zaman asimi\n"); return 0; }
    }

    // 2) Kuyruklar (tek sayfa her biri; giris boyutlari MQES'a sigar)
    n->asq_phys = pmm_alloc_pages(1); n->acq_phys = pmm_alloc_pages(1);
    n->iosq_phys = pmm_alloc_pages(1); n->iocq_phys = pmm_alloc_pages(1);
    n->prp_phys = pmm_alloc_pages(1);
    if (!n->asq_phys || !n->acq_phys || !n->iosq_phys || !n->iocq_phys || !n->prp_phys) {
        kprintf("[NVMe] bellek yok\n"); return 0;
    }
    memset((void*)(UINTN)n->asq_phys, 0, 4096); memset((void*)(UINTN)n->acq_phys, 0, 4096);
    memset((void*)(UINTN)n->iosq_phys, 0, 4096); memset((void*)(UINTN)n->iocq_phys, 0, 4096);
    memset((void*)(UINTN)n->prp_phys, 0, 4096);
    n->asq = (sqe_t*)(UINTN)n->asq_phys; n->acq = (cqe_t*)(UINTN)n->acq_phys;
    n->iosq = (sqe_t*)(UINTN)n->iosq_phys; n->iocq = (cqe_t*)(UINTN)n->iocq_phys;
    n->prp = (UINT64*)(UINTN)n->prp_phys;
    n->cq_phase = 1;

    wr32(n->bar, REG_AQA, ((n->qdepth - 1) << 16) | (n->qdepth - 1));
    *(volatile UINT64 *)(n->bar + REG_ASQ) = n->asq_phys;
    *(volatile UINT64 *)(n->bar + REG_ACQ) = n->acq_phys;

    // 3) CC: MPS=4K, IOCQES=4, IOSQES=6, EN=1
    wr32(n->bar, REG_CC, (4u << 16) | (6u << 20) | 1u);
    if (wait_csts(n->bar, 1, n->max_to_ms)) { kprintf("[NVMe] acilma zaman asimi\n"); return 0; }

    // 4) Identify Controller -> model adi
    UINT64 id_phys = pmm_alloc_pages(1); if (!id_phys) return 0;
    memset((void*)(UINTN)id_phys, 0, 4096);
    sqe_t c; memset(&c, 0, sizeof c);
    c.d0 = AOP_IDENTIFY; c.p1 = id_phys; c.c10 = 1;     // CNS=1 controller
    if (admin_cmd(&c)) { kprintf("[NVMe] identify controller hatasi\n"); return 0; }
    char model[44]; memset(model, 0, sizeof model);
    memcpy(model, (void*)(UINTN)(id_phys + 24), 40);

    // 5) I/O kuyruklari: once CQ sonra SQ (NVMe sarti)
    memset(&c, 0, sizeof c); c.d0 = AOP_CREATE_CQ; c.p1 = n->iocq_phys;
    c.c10 = ((1u << 16) | (n->qdepth - 1)); c.c11 = 1u;  // PC=1, kesme yok
    if (admin_cmd(&c)) { kprintf("[NVMe] CQ kurulamadi\n"); return 0; }
    memset(&c, 0, sizeof c); c.d0 = AOP_CREATE_SQ; c.p1 = n->iosq_phys;
    c.c10 = ((1u << 16) | (n->qdepth - 1)); c.c11 = ((1u << 16) | 1u); // CQID=1, PC=1
    if (admin_cmd(&c)) { kprintf("[NVMe] SQ kurulamadi\n"); return 0; }

    // 6) Namespace 1: boyut + sektor boyutu
    memset((void*)(UINTN)id_phys, 0, 4096);
    memset(&c, 0, sizeof c);
    c.d0 = AOP_IDENTIFY; c.d1 = 1; c.p1 = id_phys; c.c10 = 0;   // CNS=0, NSID=1
    if (admin_cmd(&c)) { kprintf("[NVMe] identify namespace hatasi\n"); return 0; }
    UINT64 nsze = *(volatile UINT64 *)(UINTN)id_phys;
    UINT8 flbas = *(volatile UINT8 *)(UINTN)(id_phys + 26);
    UINT32 lbads = flbas & 0xFu;
    pmm_free_pages(id_phys, 1);

    if (lbads != 9) {   // 2^9 = 512; blok katmani 512 sabit
        kprintf("[NVMe] %s: %llu blok ama sektor %u bayt (destek yok)\n", model, nsze, 1u << lbads);
        return 0;
    }
    g_ns_size = nsze;

    blockdev_t t; memset(&t, 0, sizeof t);
    strncpy(t.name, "nvme0", sizeof t.name - 1);
    strncpy(t.model, model, sizeof t.model - 1);
    t.sectors = g_ns_size;
    t.read = nvme_read; t.write = nvme_write;
    if (!blk_register(&t)) return 0;
    n->used = 1;
    blk_scan_partitions(blk_find("nvme0"));
    kprintf("[NVMe] %s: %llu sektor (512B) hazir\n", model, nsze);
    return 1;
}

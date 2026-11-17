// DEOS -- NVMe suruculeri: PCI bul, kontrolcuyu resetle, admin + 1 adet I/O kuyrugu kur,
// namespace 1'i blok katmanina "nvme0" olarak kaydet. Polling + global kilit (AHCI modeli).
//
// Tasarim notlari (NVMe 1.4 spesifikasyonuna gore):
//  - Her kuyrugun (admin / I/O) kendi tail, head ve phase durumu vardir.
//  - Her komut icin zaman asimi vardir; asilirsa kontrolcu "olu" isaretlenir ve sonraki
//    istekler beklemeden hata doner (gercek donanimda sonsuz donguyu engeller).
//  - Veri aktarimi 64 KB'lik sayfa-hizali bir "bounce" tamponu uzerinden yapilir; boylece
//    cagiranin tamponu hizasiz olsa bile PRP kurallari (sayfa hizasi) bozulmaz.
//  - Sinir: 512 B sektor, DMA tamponlari ilk 4 GB'te (kernel birebir esleme ile erisir).
#include "nvme.h"
#include "types.h"
#include "config.h"
#include "pci.h"
#include "blk.h"
#include "pmm.h"
#include "paging.h"
#include "thread.h"
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

#define CC_EN      (1u << 0)
#define CC_IOSQES  (6u << 16)      // SQ girdisi 2^6 = 64 bayt
#define CC_IOCQES  (4u << 20)      // CQ girdisi 2^4 = 16 bayt
#define CSTS_RDY   (1u << 0)
#define CSTS_CFS   (1u << 1)

#define AOP_CREATE_SQ  0x01
#define AOP_CREATE_CQ  0x05
#define AOP_IDENTIFY   0x06
#define IOP_FLUSH      0x00
#define IOP_WRITE      0x01
#define IOP_READ       0x02

#define BOUNCE_PAGES   16
#define MAX_SECTORS    (BOUNCE_PAGES * 4096 / 512)     // 128
#define GB4            0x100000000ULL

// 64 baytlik gonderim kuyrugu girdisi (CDW0, NSID, CDW2-3, MPTR, PRP1, PRP2, CDW10-15)
typedef struct { UINT32 d0, nsid, d2, d3; UINT64 mptr, prp1, prp2; UINT32 c10, c11, c12, c13, c14, c15; } sqe_t;
// 16 baytlik tamamlanma kuyrugu girdisi
typedef struct { UINT32 d0, d1, d2, d3; } cqe_t;
_Static_assert(sizeof(sqe_t) == 64, "sqe_t 64 bayt olmali");
_Static_assert(sizeof(cqe_t) == 16, "cqe_t 16 bayt olmali");

typedef struct {
    UINT32 qid, depth;
    sqe_t *sq;  cqe_t *cq;
    UINT64 sq_phys, cq_phys;
    UINT32 sq_tail, cq_head, phase;
    UINT16 next_cid;
} queue_t;

typedef struct {
    volatile UINT8 *bar;
    UINT32 dstrd;                // doorbell araligi = 4 << dstrd bayt
    UINT32 timeout_ms;           // komut zaman asimi (CAP.TO'dan, en az 2 sn)
    UINT64 bounce_phys; UINT8 *bounce;
    UINT64 prp_phys;    UINT64 *prp;
    queue_t aq, ioq;
    volatile int locked;
    int dead;                    // zaman asimi / CFS: kontrolcu guvenilmez
    int ready;
} nvme_t;

static nvme_t g_nvme;

static UINT32 rd32(UINT32 o) { return *(volatile UINT32 *)(g_nvme.bar + o); }
static UINT64 rd64(UINT32 o) { return *(volatile UINT64 *)(g_nvme.bar + o); }
static void   wr32(UINT32 o, UINT32 v) { *(volatile UINT32 *)(g_nvme.bar + o) = v; }
static void   wr64(UINT32 o, UINT64 v) { wr32(o, (UINT32)v); wr32(o + 4, (UINT32)(v >> 32)); }

static UINT32 ms_to_ticks(UINT32 ms) {
    UINT64 t = ((UINT64)ms * TIMER_HZ + 999) / 1000;
    return t ? (UINT32)t : 1;
}

static void lock(void)   { while (__sync_lock_test_and_set(&g_nvme.locked, 1)) thread_yield(); }
static void unlock(void) { __sync_lock_release(&g_nvme.locked); }

static void doorbell(queue_t *q, int completion, UINT32 val) {
    UINT32 off = DB_BASE + (2u * q->qid + (completion ? 1u : 0u)) * (4u << g_nvme.dstrd);
    __asm__ __volatile__ ("mfence" ::: "memory");
    wr32(off, val);
}

// CSTS.RDY == want olana kadar bekler. 0 = tamam, -1 = zaman asimi veya CFS.
static int wait_rdy(UINT32 want) {
    UINT64 deadline = timer_get_ticks() + ms_to_ticks(g_nvme.timeout_ms);
    for (;;) {
        UINT32 s = rd32(REG_CSTS);
        if ((s & CSTS_RDY) == want) return 0;
        if (s & CSTS_CFS) return -1;
        if (timer_get_ticks() > deadline) return -1;
        __asm__ __volatile__ ("pause");
    }
}

// Komutu kuyruga koyar ve tamamlanmasini bekler. 0 = basarili, -1 = hata durumu, -2 = zaman asimi.
static int submit(queue_t *q, const sqe_t *cmd) {
    nvme_t *n = &g_nvme;
    if (n->dead) return -2;
    UINT16 cid = ++q->next_cid;
    sqe_t e = *cmd;
    e.d0 = (e.d0 & 0xFFFFu) | ((UINT32)cid << 16);
    ((volatile sqe_t *)q->sq)[q->sq_tail] = e;
    q->sq_tail = (q->sq_tail + 1) % q->depth;
    doorbell(q, 0, q->sq_tail);

    UINT64 deadline = timer_get_ticks() + ms_to_ticks(n->timeout_ms);
    for (;;) {
        volatile cqe_t *c = &((volatile cqe_t *)q->cq)[q->cq_head];
        UINT32 d3 = c->d3;
        if (((d3 >> 16) & 1u) == q->phase) {                    // yeni girdi (phase eslesti)
            q->cq_head = (q->cq_head + 1) % q->depth;
            if (q->cq_head == 0) q->phase ^= 1u;
            doorbell(q, 1, q->cq_head);                         // YENI head degeri
            UINT32 status = d3 >> 17;
            if ((UINT16)(d3 & 0xFFFFu) != cid) { n->dead = 1; return -2; }   // beklenmeyen CID: senkron bozuk
            if (status) {
                kprintf("[NVMe] komut hatasi: opcode 0x%x, SCT=%u SC=0x%x\n",
                        (unsigned)(cmd->d0 & 0xFFu), (status >> 8) & 7u, status & 0xFFu);
                return -1;
            }
            return 0;
        }
        if (timer_get_ticks() > deadline || (rd32(REG_CSTS) & CSTS_CFS)) {
            kprintf("[NVMe] komut zaman asimi/CFS (opcode 0x%x) -- aygit devre disi\n", (unsigned)(cmd->d0 & 0xFFu));
            n->dead = 1;
            return -2;
        }
        __asm__ __volatile__ ("pause");
    }
}

// ---------------------------------------------------------------- blok G/C
static void prp_setup(sqe_t *c, UINT32 bytes) {
    nvme_t *n = &g_nvme;
    UINT32 nprp = (bytes + 4095) / 4096;
    c->prp1 = n->bounce_phys;
    c->prp2 = 0;
    if (nprp == 2) c->prp2 = n->bounce_phys + 4096;
    else if (nprp > 2) {
        for (UINT32 i = 1; i < nprp; i++) n->prp[i - 1] = n->bounce_phys + (UINT64)i * 4096;
        c->prp2 = n->prp_phys;
    }
}

static int rw(int is_write, UINT64 lba, UINT32 count, void *buf) {
    nvme_t *n = &g_nvme;
    if (!count) return -1;
    UINT8 *p = (UINT8 *)buf;
    lock();
    int r = 0;
    while (count && r == 0) {
        UINT32 k = count > MAX_SECTORS ? MAX_SECTORS : count;
        sqe_t c; memset(&c, 0, sizeof c);
        c.d0 = is_write ? IOP_WRITE : IOP_READ;
        c.nsid = 1;
        prp_setup(&c, k * 512);
        c.c10 = (UINT32)lba; c.c11 = (UINT32)(lba >> 32); c.c12 = k - 1;
        if (is_write) memcpy(n->bounce, p, (UINTN)k * 512);
        r = submit(&n->ioq, &c);
        if (r == 0 && is_write) {                       // AHCI davranisi gibi: her yazmadan sonra flush
            sqe_t f; memset(&f, 0, sizeof f);
            f.d0 = IOP_FLUSH; f.nsid = 1;
            r = submit(&n->ioq, &f);
        }
        if (r == 0 && !is_write) memcpy(p, n->bounce, (UINTN)k * 512);
        p += (UINTN)k * 512; lba += k; count -= k;
    }
    unlock();
    return r == 0 ? 0 : -1;
}

static int nvme_read(blockdev_t *d, UINT64 lba, UINT32 count, void *buf)               { (void)d; return rw(0, lba, count, buf); }
static int nvme_write(blockdev_t *d, UINT64 lba, UINT32 count, const void *buf)        { (void)d; return rw(1, lba, count, (void *)buf); }

// ---------------------------------------------------------------- baslatma
static UINT64 alloc_dma(UINT64 pages) {
    UINT64 p = pmm_alloc_pages(pages);
    if (!p) return 0;
    if (p + pages * 4096 > GB4) { pmm_free_pages(p, pages); return 0; }
    memset((void *)(UINTN)p, 0, (UINTN)(pages * 4096));
    return p;
}

static int queue_alloc(queue_t *q, UINT32 qid, UINT32 depth) {
    q->qid = qid; q->depth = depth; q->sq_tail = q->cq_head = 0; q->phase = 1; q->next_cid = 0;
    q->sq_phys = alloc_dma(1); q->cq_phys = alloc_dma(1);
    if (!q->sq_phys || !q->cq_phys) return -1;
    q->sq = (sqe_t *)(UINTN)q->sq_phys; q->cq = (cqe_t *)(UINTN)q->cq_phys;
    return 0;
}

static void free_all(void) {
    nvme_t *n = &g_nvme;
    if (n->aq.sq_phys)  pmm_free_pages(n->aq.sq_phys, 1);
    if (n->aq.cq_phys)  pmm_free_pages(n->aq.cq_phys, 1);
    if (n->ioq.sq_phys) pmm_free_pages(n->ioq.sq_phys, 1);
    if (n->ioq.cq_phys) pmm_free_pages(n->ioq.cq_phys, 1);
    if (n->prp_phys)    pmm_free_pages(n->prp_phys, 1);
    if (n->bounce_phys) pmm_free_pages(n->bounce_phys, BOUNCE_PAGES);
}

static void fail_cleanup(void) {
    if (g_nvme.bar) {
        wr32(REG_CC, 0);                 // kontrolcuyu sustur: DMA kalmasin
        wait_rdy(0);
        free_all();
    }
    memset(&g_nvme, 0, sizeof g_nvme);
}

int nvme_init(void) {
    const pci_dev_t *pci = pci_find_class(0x01, 0x08, 0x02);
    if (!pci) return 0;
    pci_enable(pci);

    UINT32 lo = pci_read32(pci, 0x10), hi = pci_read32(pci, 0x14);
    if (lo & 1u) { kprintf("[NVMe] BAR0 MMIO degil\n"); return 0; }
    UINT64 bar = (((lo & 0x6u) == 0x4u) ? ((UINT64)hi << 32) : 0) | (lo & ~0xFULL);
    if (!bar) { kprintf("[NVMe] BAR0 atanmamis\n"); return 0; }

    nvme_t *n = &g_nvme;
    memset(n, 0, sizeof *n);
    // doorbell'lar 0x1000'den baslar; 4 adet (admin+I/O SQ/CQ) icin yeterli alani esle
    n->bar = (volatile UINT8 *)mmio_map(bar, 0x4000);
    if (!n->bar) { kprintf("[NVMe] BAR eslenemedi\n"); memset(n, 0, sizeof *n); return 0; }

    UINT64 cap = rd64(REG_CAP);
    if (cap == ~0ULL) { kprintf("[NVMe] CAP okunamadi (aygit yok/cevap vermiyor)\n"); memset(n, 0, sizeof *n); return 0; }
    UINT32 mqes   = (UINT32)(cap & 0xFFFF) + 1;
    UINT32 to_ms  = (UINT32)((cap >> 24) & 0xFF) * 500u;
    n->dstrd      = (UINT32)((cap >> 32) & 0xF);
    UINT32 mpsmin = (UINT32)((cap >> 48) & 0xF);
    n->timeout_ms = to_ms < 2000 ? 2000 : to_ms;
    if (n->dstrd > 4) { kprintf("[NVMe] doorbell araligi cok buyuk (DSTRD=%u)\n", n->dstrd); memset(n, 0, sizeof *n); return 0; }
    if (mpsmin != 0)  { kprintf("[NVMe] 4 KB sayfa desteklenmiyor (MPSMIN=%u)\n", mpsmin); memset(n, 0, sizeof *n); return 0; }
    if (mqes < 2)     { kprintf("[NVMe] kuyruk boyutu yetersiz\n"); memset(n, 0, sizeof *n); return 0; }
    UINT32 depth = mqes < 32 ? mqes : 32;

    // 1) UEFI/firmware kontrolcuyu acik birakmis olabilir: once kapat (CC.EN=0) ve RDY=0 bekle.
    if (rd32(REG_CC) & CC_EN) {
        wr32(REG_CC, rd32(REG_CC) & ~CC_EN);
        if (wait_rdy(0)) { kprintf("[NVMe] kapatma (reset) zaman asimi\n"); memset(n, 0, sizeof *n); return 0; }
    }

    // 2) Bellek: admin + I/O kuyruklari, PRP listesi, bounce tamponu
    n->bounce_phys = alloc_dma(BOUNCE_PAGES);
    n->prp_phys    = alloc_dma(1);
    if (!n->bounce_phys || !n->prp_phys || queue_alloc(&n->aq, 0, depth) || queue_alloc(&n->ioq, 1, depth)) {
        kprintf("[NVMe] bellek ayrilamadi (DMA < 4 GB gerekli)\n");
        fail_cleanup(); return 0;
    }
    n->bounce = (UINT8 *)(UINTN)n->bounce_phys;
    n->prp    = (UINT64 *)(UINTN)n->prp_phys;

    // 3) Admin kuyruk kaydi + etkinlestirme
    wr32(REG_AQA, ((depth - 1) << 16) | (depth - 1));
    wr64(REG_ASQ, n->aq.sq_phys);
    wr64(REG_ACQ, n->aq.cq_phys);
    wr32(REG_CC, CC_IOSQES | CC_IOCQES | CC_EN);       // MPS=0 (4 KB), CSS=0 (NVM), AMS=0 (round robin)
    if (wait_rdy(1)) { kprintf("[NVMe] acilma zaman asimi / CFS\n"); fail_cleanup(); return 0; }
    n->ready = 1;

    // 4) Identify Controller -> model adi
    UINT64 id_phys = alloc_dma(1);
    if (!id_phys) { fail_cleanup(); return 0; }
    UINT8 *id = (UINT8 *)(UINTN)id_phys;
    sqe_t c; memset(&c, 0, sizeof c);
    c.d0 = AOP_IDENTIFY; c.prp1 = id_phys; c.c10 = 1;            // CNS=1: controller
    if (submit(&n->aq, &c)) { kprintf("[NVMe] identify controller hatasi\n"); pmm_free_pages(id_phys, 1); fail_cleanup(); return 0; }
    char model[44]; memset(model, 0, sizeof model);
    memcpy(model, id + 24, 40);
    for (int i = 39; i >= 0 && (model[i] == ' ' || model[i] == 0); i--) model[i] = 0;

    // 5) I/O kuyruklari: once CQ, sonra SQ (spesifikasyon sarti). CDW10 = (QSIZE-1)<<16 | QID.
    memset(&c, 0, sizeof c);
    c.d0 = AOP_CREATE_CQ; c.prp1 = n->ioq.cq_phys;
    c.c10 = ((depth - 1) << 16) | 1u; c.c11 = 1u;                // PC=1 (bitisik), IEN=0 (kesme yok)
    if (submit(&n->aq, &c)) { kprintf("[NVMe] I/O CQ kurulamadi\n"); pmm_free_pages(id_phys, 1); fail_cleanup(); return 0; }
    memset(&c, 0, sizeof c);
    c.d0 = AOP_CREATE_SQ; c.prp1 = n->ioq.sq_phys;
    c.c10 = ((depth - 1) << 16) | 1u; c.c11 = (1u << 16) | 1u;  // CQID=1, PC=1
    if (submit(&n->aq, &c)) { kprintf("[NVMe] I/O SQ kurulamadi\n"); pmm_free_pages(id_phys, 1); fail_cleanup(); return 0; }

    // 6) Namespace 1: boyut + gercek sektor boyutu (FLBAS -> LBAF tablosu -> LBADS)
    memset(id, 0, 4096);
    memset(&c, 0, sizeof c);
    c.d0 = AOP_IDENTIFY; c.nsid = 1; c.prp1 = id_phys; c.c10 = 0;    // CNS=0: namespace
    if (submit(&n->aq, &c)) { kprintf("[NVMe] identify namespace hatasi\n"); pmm_free_pages(id_phys, 1); fail_cleanup(); return 0; }
    UINT64 nsze = *(volatile UINT64 *)id;
    UINT32 fidx = id[26] & 0xFu;                       // FLBAS[3:0] = bicim indeksi
    UINT32 lbaf = *(volatile UINT32 *)(id + 128 + 4 * fidx);
    UINT32 lbads = (lbaf >> 16) & 0xFFu;
    pmm_free_pages(id_phys, 1);

    if (nsze == 0) { kprintf("[NVMe] %s: namespace 1 bos/aktif degil\n", model); fail_cleanup(); return 0; }
    if (lbads != 9) {                                  // 2^9 = 512; blok katmani 512 sabit
        kprintf("[NVMe] %s: sektor %u bayt (yalnizca 512 destekli)\n", model, 1u << (lbads & 31));
        fail_cleanup(); return 0;
    }

    blockdev_t t; memset(&t, 0, sizeof t);
    strncpy(t.name, "nvme0", sizeof t.name - 1);
    strncpy(t.model, model, sizeof t.model - 1);
    t.sectors = nsze;
    t.read = nvme_read; t.write = nvme_write;
    if (!blk_register(&t)) { fail_cleanup(); return 0; }
    blk_scan_partitions(blk_find("nvme0"));
    kprintf("[NVMe] %s: %llu sektor (512B) hazir\n", model, nsze);
    return 1;
}

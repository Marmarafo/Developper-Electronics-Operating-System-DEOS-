// DEOS -- blok aygit katmani: aygit kaydi, sinir denetimi, GPT/MBR bolum ayristirma.
#include "types.h"
#include "blk.h"
#include "heap.h"
#include "kprintf.h"
#include "string.h"

#define MAX_BLK 16
static blockdev_t g_blk[MAX_BLK];
static int g_n;

blockdev_t *blk_register(const blockdev_t *t) {
    if (g_n >= MAX_BLK) return 0;
    g_blk[g_n] = *t;
    return &g_blk[g_n++];
}

int blk_count(void) { return g_n; }
blockdev_t *blk_get(int i) { return (i >= 0 && i < g_n) ? &g_blk[i] : 0; }

blockdev_t *blk_find(const char *name) {
    for (int i = 0; i < g_n; i++) if (!strcmp(g_blk[i].name, name)) return &g_blk[i];
    return 0;
}

int blk_read(blockdev_t *d, UINT64 lba, UINT32 count, void *buf) {
    if (!d || !d->read || count == 0 || lba + count < lba || lba + count > d->sectors) return -1;
    return d->read(d, lba, count, buf);
}

int blk_write(blockdev_t *d, UINT64 lba, UINT32 count, const void *buf) {
    if (!d || !d->write || count == 0 || lba + count < lba || lba + count > d->sectors) return -1;
    return d->write(d, lba, count, buf);
}

// ---- bolum aygitlari: istekleri ofsetle ust diske iletir ----
static int part_read(blockdev_t *d, UINT64 lba, UINT32 count, void *buf) {
    return blk_read(d->parent, d->offset + lba, count, buf);
}
static int part_write(blockdev_t *d, UINT64 lba, UINT32 count, const void *buf) {
    return blk_write(d->parent, d->offset + lba, count, buf);
}

static void add_partition(blockdev_t *disk, int idx, UINT64 first, UINT64 count) {
    if (first >= disk->sectors || count == 0 || first + count > disk->sectors) return;
    blockdev_t p;
    memset(&p, 0, sizeof p);
    ksnprintf(p.name, sizeof p.name, "%sp%d", disk->name, idx);
    memcpy(p.model, disk->model, sizeof p.model);
    p.sectors = count; p.parent = disk; p.offset = first;
    p.read = part_read; p.write = part_write;
    blk_register(&p);
}

static int looks_like_fat(const UINT8 *s) {
    if (s[510] != 0x55 || s[511] != 0xAA) return 0;
    if (s[0] != 0xEB && s[0] != 0xE9) return 0;
    if (s[11] != 0 || s[12] != 2) return 0;                         // 512 bayt/sektor
    return !memcmp(s + 0x52, "FAT32", 5) || !memcmp(s + 0x36, "FAT16", 5) || !memcmp(s + 0x36, "FAT12", 5);
}

void blk_scan_partitions(blockdev_t *disk) {
    UINT8 *s = kmalloc(1024);
    if (!s) return;
    if (blk_read(disk, 0, 1, s) != 0) { kfree(s); return; }
    if (looks_like_fat(s)) { kfree(s); return; }                    // bolumsuz (superfloppy) FAT

    int n = 0;
    if (blk_read(disk, 1, 1, s + 512) == 0 && !memcmp(s + 512, "EFI PART", 8)) {         // GPT
        UINT64 ent_lba = *(UINT64*)(s + 512 + 72);
        UINT32 cnt = *(UINT32*)(s + 512 + 80), esz = *(UINT32*)(s + 512 + 84);
        if (esz >= 128 && esz <= 512 && cnt <= 128) {
            UINT8 *e = kmalloc(512);
            for (UINT32 i = 0; e && i < cnt; i++) {
                UINT64 off = (UINT64)i * esz;
                if (i == 0 || off % 512 < esz) { /* yeni sektor gerekebilir */ }
                if (blk_read(disk, ent_lba + off / 512, 1, e) != 0) break;
                UINT8 *ent = e + (off % 512);
                int used = 0; for (int k = 0; k < 16; k++) if (ent[k]) used = 1;
                if (!used) continue;
                add_partition(disk, ++n, *(UINT64*)(ent + 32), *(UINT64*)(ent + 40) - *(UINT64*)(ent + 32) + 1);
            }
            kfree(e);
        }
    } else if (s[510] == 0x55 && s[511] == 0xAA) {                                       // MBR
        for (int i = 0; i < 4; i++) {
            const UINT8 *p = s + 446 + 16 * i;
            UINT8 type = p[4];
            if (!type || type == 0x05 || type == 0x0F || type == 0x85) continue;         // bos / genisletilmis
            add_partition(disk, ++n, *(UINT32*)(p + 8), *(UINT32*)(p + 12));
        }
    }
    kfree(s);
}

void blk_mark_removed(blockdev_t *d) { d->read = 0; d->write = 0; }

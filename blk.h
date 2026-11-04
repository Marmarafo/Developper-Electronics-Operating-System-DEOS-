#ifndef DEOS_BLK_H
#define DEOS_BLK_H
#include "types.h"

// Blok aygit: 512 baytlik sektorlerden olusan disk veya bolum.
typedef struct blockdev {
    char   name[16];                 // "sd0", "sd0p1" ...
    char   model[44];
    UINT64 sectors;
    int    (*read)(struct blockdev *d, UINT64 lba, UINT32 count, void *buf);              // 0 = basarili
    int    (*write)(struct blockdev *d, UINT64 lba, UINT32 count, const void *buf);
    void  *priv;
    struct blockdev *parent;         // bolumse: bagli oldugu disk
    UINT64 offset;                   // bolumse: diskteki baslangic sektoru
} blockdev_t;

blockdev_t *blk_register(const blockdev_t *tmpl);     // kopyalar; NULL = tablo dolu
int         blk_count(void);
blockdev_t *blk_get(int index);
blockdev_t *blk_find(const char *name);

int blk_read(blockdev_t *d, UINT64 lba, UINT32 count, void *buf);              // sinir denetimli
int blk_write(blockdev_t *d, UINT64 lba, UINT32 count, const void *buf);

// GPT / MBR bolumlerini bulup "sd0p1".. aygitlari olarak kaydeder (superfloppy FAT'e dokunmaz).
void blk_scan_partitions(blockdev_t *disk);

#endif

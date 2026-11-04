#ifndef DEOS_FAT_H
#define DEOS_FAT_H
#include "types.h"
#include "blk.h"

typedef struct fatvol fatvol_t;

typedef struct {
    char   name[256];
    UINT32 size;
    UINT32 cluster;       // ilk kume (0 = bos dosya / kok)
    UINT16 wdate, wtime;  // FAT bicimi
    int    is_dir;
    UINT8  attr;
    UINT32 dir_cl, idx, lfn_idx;   // girdinin konumu (dizin kumesi, SFN indeksi, ilk LFN indeksi)
} fat_entry_t;

fatvol_t *fat_mount(blockdev_t *dev);                    // FAT16/FAT32; NULL = FAT degil
const char *fat_label(fatvol_t *v);
blockdev_t *fat_device(fatvol_t *v);
UINT32 fat_cluster_bytes(fatvol_t *v);
int fat_type(fatvol_t *v);

// Yol: "/" ile baslayan, normallestirilmis (".." yok), hacmin kokune gore.
int fat_stat(fatvol_t *v, const char *path, fat_entry_t *out);                    // 0 = bulundu
int fat_readdir(fatvol_t *v, const char *path, int index, fat_entry_t *out);      // 0 = girdi, 1 = son, -1 = hata
long long fat_read(fatvol_t *v, const fat_entry_t *f, UINT64 off, void *buf, UINT64 len);   // okunan bayt | -1

// ---- yazma ----  (hata kodlari: -1 yok/gecersiz, -2 zaten var, -3 dolu degil dizin / dizin dolu, -4 yer yok, -5 G/C hatasi)
int fat_create(fatvol_t *v, const char *path, int is_dir);                        // bos dosya veya dizin
long long fat_write(fatvol_t *v, const char *path, UINT64 off, const void *buf, UINT64 len);   // off <= dosya boyu
int fat_truncate(fatvol_t *v, const char *path, UINT64 size);                     // yalnizca kucultme (0 dahil)
int fat_remove(fatvol_t *v, const char *path);                                    // dosya ya da BOS dizin
int fat_rename(fatvol_t *v, const char *from, const char *to);                    // ayni hacim icinde tasi/adlandir
int fat_space(fatvol_t *v, UINT64 *total_clusters, UINT64 *free_clusters);
void fat_sync(fatvol_t *v);                                                       // FSInfo'yu diske yaz

#endif

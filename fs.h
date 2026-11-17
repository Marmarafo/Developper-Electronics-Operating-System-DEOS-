#ifndef DEOS_FS_H
#define DEOS_FS_H
#include "types.h"
#include "fat.h"
#include "blk.h"

typedef fat_entry_t fs_dirent_t;

// Disklerdeki FAT hacimlerini bulup baglar: ilk hacim "/", digerleri "/mnt/<aygit>". Baglanan hacim sayisi.
int  fs_init(void);

// Yol islemleri (mutlak ya da cwd'ye gore). Sonuc normallestirilmis mutlak yoldur.
void fs_normalize(const char *cwd, const char *path, char *out, UINT32 cap);

// ---- okuma ----
int  fs_stat(const char *abs_path, fs_dirent_t *out);                              // 0 = var
int  fs_readdir(const char *abs_path, int index, fs_dirent_t *out);                // 0 girdi, 1 son, -1 hata
long long fs_read(const char *abs_path, UINT64 off, void *buf, UINT64 len);        // okunan bayt | -1
void *fs_read_all(const char *abs_path, UINT64 *size);                             // kmalloc ile; kfree edin

// ---- yazma (hata: negatif) ----
int  fs_create(const char *abs_path, int is_dir);                                  // bos dosya / dizin
long long fs_write(const char *abs_path, UINT64 off, const void *buf, UINT64 len);
int  fs_write_file(const char *abs_path, const void *buf, UINT64 len);             // olustur/uzerine yaz (tum icerik)
int  fs_remove(const char *abs_path);                                              // dosya ya da bos dizin
int  fs_rename(const char *from, const char *to);                                  // ayni hacim icinde
int  fs_space(const char *abs_path, UINT64 *total_bytes, UINT64 *free_bytes);
const char *fs_strerror(int err);

int  fs_mount_device(blockdev_t *d);                     // bir aygittaki FAT hacmini baglar (1 = baglandi)
void fs_unmount_device(blockdev_t *d);                   // aygit cikarilirken
int  fs_mount_count(void);
const char *fs_mount_info(int i, const char **mountpoint, const char **dev, int *type);

#endif

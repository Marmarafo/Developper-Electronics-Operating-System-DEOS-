// DEOS -- dosya sistemi katmani: baglama noktalari, yol normallestirme, FAT hacimlerine yonlendirme.
// Her genel islev tek bir global kilitle korunur (GUI, kabuk ve kullanici sureclerinin es zamanli erisimi icin).
#include "types.h"
#include "fs.h"
#include "blk.h"
#include "heap.h"
#include "thread.h"
#include "kprintf.h"
#include "string.h"
#include "cpu.h"

#define MAX_MOUNT 8
static struct { char mp[24]; char dev[16]; fatvol_t *vol; } g_m[MAX_MOUNT];
static int g_nm;
static volatile int g_lock;

static void lock(void) {
    for (;;) {
        UINT64 f = irq_save();
        if (!g_lock) { g_lock = 1; irq_restore(f); return; }
        irq_restore(f);
        thread_yield();
    }
}
static void unlock(void) { g_lock = 0; }

int fs_init(void) {
    lock();
    g_nm = 0;
    for (int i = 0; i < blk_count() && g_nm < MAX_MOUNT; i++) {
        blockdev_t *d = blk_get(i);
        fatvol_t *v = fat_mount(d);
        if (!v) continue;
        if (g_nm == 0) { g_m[0].mp[0] = '/'; g_m[0].mp[1] = 0; }
        else ksnprintf(g_m[g_nm].mp, sizeof g_m[g_nm].mp, "/mnt/%s", d->name);
        size_t n = strlen(d->name); if (n > 15) n = 15;
        memcpy(g_m[g_nm].dev, d->name, n); g_m[g_nm].dev[n] = 0;
        g_m[g_nm].vol = v;
        g_nm++;
    }
    int r = g_nm;
    unlock();
    return r;
}

int fs_mount_count(void) { return g_nm; }

const char *fs_mount_info(int i, const char **mp, const char **dev, int *type) {
    if (i < 0 || i >= g_nm) return 0;
    *mp = g_m[i].mp; *dev = g_m[i].dev; *type = fat_type(g_m[i].vol);
    return fat_label(g_m[i].vol);
}

void fs_normalize(const char *cwd, const char *path, char *out, UINT32 cap) {
    char tmp[256];
    tmp[0] = 0;
    if (path[0] != '/') { size_t cl = strlen(cwd); if (cl >= sizeof tmp) cl = sizeof tmp - 1; memcpy(tmp, cwd, cl); tmp[cl] = 0; }
    UINT32 n = (UINT32)strlen(tmp);
    if (n + 1 < sizeof tmp) { tmp[n++] = '/'; tmp[n] = 0; }
    UINT32 m = (UINT32)strlen(path);
    if (n + m < sizeof tmp) memcpy(tmp + n, path, m + 1);

    char res[256];
    UINT32 len = 0;
    res[0] = 0;
    const char *p = tmp;
    while (*p) {
        while (*p == '/') p++;
        char comp[128];
        int k = 0;
        while (*p && *p != '/' && k < 127) comp[k++] = *p++;
        comp[k] = 0;
        if (!k || !strcmp(comp, ".")) continue;
        if (!strcmp(comp, "..")) { while (len > 0 && res[len - 1] != '/') len--; if (len > 0) len--; res[len] = 0; continue; }
        if (len + k + 2 >= sizeof res) break;
        res[len++] = '/';
        memcpy(res + len, comp, k); len += k; res[len] = 0;
    }
    if (len == 0) { res[0] = '/'; res[1] = 0; }
    UINT32 rl = (UINT32)strlen(res);
    if (rl >= cap) rl = cap - 1;
    memcpy(out, res, rl); out[rl] = 0;
}

// Yola karsilik gelen hacmi ve hacim icindeki yolu bulur.
static fatvol_t *route(const char *path, const char **rest) {
    for (int i = 1; i < g_nm; i++) {
        size_t l = strlen(g_m[i].mp);
        if (!strncmp(path, g_m[i].mp, l) && (path[l] == 0 || path[l] == '/')) { *rest = path[l] ? path + l : "/"; return g_m[i].vol; }
    }
    if (g_nm == 0) return 0;
    *rest = path;
    return g_m[0].vol;
}

static int is_mnt_dir(const char *p) { return g_nm > 1 && !strcmp(p, "/mnt"); }

static void synth_dir(fs_dirent_t *e, const char *name) {
    memset(e, 0, sizeof *e);
    size_t n = strlen(name); if (n > 255) n = 255;
    memcpy(e->name, name, n);
    e->is_dir = 1; e->attr = 0x10;
}

static int stat_nolock(const char *p, fs_dirent_t *out) {
    if (is_mnt_dir(p)) { synth_dir(out, "mnt"); return 0; }
    const char *rest; fatvol_t *v = route(p, &rest);
    return v ? fat_stat(v, rest, out) : -1;
}

int fs_stat(const char *p, fs_dirent_t *out) { lock(); int r = stat_nolock(p, out); unlock(); return r; }

int fs_readdir(const char *p, int index, fs_dirent_t *out) {
    lock();
    int r;
    if (is_mnt_dir(p)) {                                              // sanal dizin: bagli hacimler
        if (index >= 0 && index + 1 < g_nm) { synth_dir(out, g_m[index + 1].dev); r = 0; } else r = 1;
    } else {
        const char *rest; fatvol_t *v = route(p, &rest);
        r = v ? fat_readdir(v, rest, index, out) : -1;
        if (r == 1 && g_nm > 1 && !strcmp(p, "/")) {                  // kokte, gercek girdilerden sonra sanal "mnt"
            fs_dirent_t tmp;
            if (fat_stat(g_m[0].vol, "/mnt", &tmp) != 0) {
                int real = 0;
                while (fat_readdir(g_m[0].vol, "/", real, &tmp) == 0) real++;
                if (index == real) { synth_dir(out, "mnt"); r = 0; }
            }
        }
    }
    unlock();
    return r;
}

long long fs_read(const char *p, UINT64 off, void *buf, UINT64 len) {
    lock();
    const char *rest; fatvol_t *v = route(p, &rest);
    long long r = -1;
    fat_entry_t e;
    if (v && fat_stat(v, rest, &e) == 0) r = fat_read(v, &e, off, buf, len);
    unlock();
    return r;
}

void *fs_read_all(const char *p, UINT64 *size) {
    lock();
    const char *rest; fatvol_t *v = route(p, &rest);
    fat_entry_t e;
    UINT8 *buf = 0;
    if (v && fat_stat(v, rest, &e) == 0 && !e.is_dir) {
        buf = kmalloc(e.size ? e.size : 1);
        if (buf && e.size && fat_read(v, &e, 0, buf, e.size) != (long long)e.size) { kfree(buf); buf = 0; }
        if (buf) *size = e.size;
    }
    unlock();
    return buf;
}

// ------------------------------------------------------------------ yazma

static fatvol_t *wroute(const char *p, const char **rest) {
    if (is_mnt_dir(p) || !strcmp(p, "/")) return 0;                   // sanal/kok dizinin kendisi degistirilemez
    return route(p, rest);
}

int fs_create(const char *p, int is_dir) {
    lock();
    const char *rest; fatvol_t *v = wroute(p, &rest);
    int r = v ? fat_create(v, rest, is_dir) : -1;
    if (v) fat_sync(v);
    unlock();
    return r;
}

long long fs_write(const char *p, UINT64 off, const void *buf, UINT64 len) {
    lock();
    const char *rest; fatvol_t *v = wroute(p, &rest);
    long long r = v ? fat_write(v, rest, off, buf, len) : -1;
    if (v) fat_sync(v);
    unlock();
    return r;
}

int fs_write_file(const char *p, const void *buf, UINT64 len) {
    lock();
    const char *rest; fatvol_t *v = wroute(p, &rest);
    int r = -1;
    if (v) {
        fat_entry_t e;
        if (fat_stat(v, rest, &e) == 0) { r = e.is_dir ? -1 : fat_truncate(v, rest, 0); }
        else r = fat_create(v, rest, 0);
        if (r == 0 && len) { long long w = fat_write(v, rest, 0, buf, len); r = (w == (long long)len) ? 0 : (w < 0 ? (int)w : -4); }
        fat_sync(v);
    }
    unlock();
    return r;
}

int fs_remove(const char *p) {
    lock();
    const char *rest; fatvol_t *v = wroute(p, &rest);
    int r = v ? fat_remove(v, rest) : -1;
    if (v) fat_sync(v);
    unlock();
    return r;
}

int fs_rename(const char *from, const char *to) {
    lock();
    const char *r1, *r2;
    fatvol_t *a = wroute(from, &r1), *b = wroute(to, &r2);
    int r = (a && a == b) ? fat_rename(a, r1, r2) : -1;
    if (a) fat_sync(a);
    unlock();
    return r;
}

int fs_space(const char *p, UINT64 *total, UINT64 *freeb) {
    lock();
    const char *rest; fatvol_t *v = route(p, &rest);
    int r = -1;
    if (v) {
        UINT64 t, f;
        fat_space(v, &t, &f);
        *total = t * fat_cluster_bytes(v); *freeb = f * fat_cluster_bytes(v);
        r = 0;
    }
    unlock();
    return r;
}

const char *fs_strerror(int e) {
    switch (e) {
    case 0:  return "tamam";
    case -1: return "yol gecersiz veya bulunamadi";
    case -2: return "zaten var";
    case -3: return "dizin bos degil / dizin dolu";
    case -4: return "diskte yer yok";
    case -5: return "disk G/C hatasi";
    default: return "hata";
    }
}

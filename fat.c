// DEOS -- FAT16 / FAT32 dosya sistemi: okuma ve yazma. Uzun dosya adlari (LFN) okunur ve uretilir.
// Tum islemler yol tabanlidir; es zamanli erisim fs.c'deki kilitle korunur.
#include "types.h"
#include "fat.h"
#include "heap.h"
#include "kprintf.h"
#include "string.h"
#include "rtc.h"

struct fatvol {
    blockdev_t *dev;
    int type;                                         // 16 | 32
    UINT32 spc, reserved, nfats, fatsz, root_cl, root_entries, total, data_lba, root_lba, root_secs, clusters;
    char label[12];
    UINT8  fsec[512];  UINT64 fsec_lba;  int fsec_ok;     // FAT tablosu sektor onbellegi
    UINT8  dsec[512];  UINT64 dsec_lba;  int dsec_ok;     // dizin sektor onbellegi
    UINT8  xsec[512];  UINT64 xsec_lba;  int xsec_ok;     // dosya verisi sektor onbellegi
    UINT32 free_hint;
    UINT64 fsinfo_lba;                                    // 0 = FSInfo yok (FAT16)
    UINT32 free_count, next_free;                         // FSInfo degerleri (0xFFFFFFFF = bilinmiyor)
    int    fsinfo_dirty;
};

static UINT16 u16(const UINT8 *p) { return (UINT16)(p[0] | (p[1] << 8)); }
static UINT32 u32(const UINT8 *p) { return (UINT32)p[0] | ((UINT32)p[1] << 8) | ((UINT32)p[2] << 16) | ((UINT32)p[3] << 24); }
static void put16(UINT8 *p, UINT16 v) { p[0] = (UINT8)v; p[1] = (UINT8)(v >> 8); }
static void put32(UINT8 *p, UINT32 v) { p[0] = (UINT8)v; p[1] = (UINT8)(v >> 8); p[2] = (UINT8)(v >> 16); p[3] = (UINT8)(v >> 24); }

fatvol_t *fat_mount(blockdev_t *dev) {
    UINT8 *s = kmalloc(512);
    if (!s) return 0;
    if (blk_read(dev, 0, 1, s) != 0 || s[510] != 0x55 || s[511] != 0xAA || u16(s + 11) != 512 || s[13] == 0) { kfree(s); return 0; }

    fatvol_t *v = kcalloc(1, sizeof *v);
    if (!v) { kfree(s); return 0; }
    v->dev = dev;
    v->spc = s[13]; v->reserved = u16(s + 14); v->nfats = s[16]; v->root_entries = u16(s + 17);
    UINT32 total16 = u16(s + 19), fatsz16 = u16(s + 22);
    v->total = total16 ? total16 : u32(s + 32);
    v->fatsz = fatsz16 ? fatsz16 : u32(s + 36);
    v->root_cl = u32(s + 44);
    v->root_secs = (v->root_entries * 32 + 511) / 512;
    v->root_lba = v->reserved + v->nfats * v->fatsz;
    v->data_lba = v->root_lba + v->root_secs;
    v->clusters = v->total > v->data_lba ? (v->total - v->data_lba) / v->spc : 0;
    // Tur tespiti: FAT32'nin BPB'sinde FATSz16 = 0 ve kok girdi sayisi 0'dir. (Kume sayisi esigi 32 MB
    // gibi kucuk FAT32 hacimlerinde YANLIS sonuc verir; bu yuzden once BPB'ye bakilir.)
    if (fatsz16 == 0 && v->root_entries == 0) v->type = 32;
    else v->type = v->clusters < 4085 ? 12 : 16;
    int lab = (v->type == 32) ? 71 : 43;
    memcpy(v->label, s + lab, 11); v->label[11] = 0;
    for (int i = 10; i >= 0 && (v->label[i] == ' ' || v->label[i] == 0); i--) v->label[i] = 0;
    v->free_hint = 2;
    v->free_count = v->next_free = 0xFFFFFFFFu;
    if (v->type == 32) {                                           // FSInfo (bos kume sayaci)
        UINT16 fi = u16(s + 48);
        if (fi && fi < v->reserved) {
            UINT8 *f = kmalloc(512);
            if (f && blk_read(dev, fi, 1, f) == 0 && u32(f) == 0x41615252u && u32(f + 484) == 0x61417272u) {
                v->fsinfo_lba = fi;
                v->free_count = u32(f + 488);
                v->next_free = u32(f + 492);
            }
            kfree(f);
        }
    }
    kfree(s);
    if (v->type == 12 || v->nfats == 0 || v->fatsz == 0 || (v->type == 32 && v->root_cl < 2)) { kfree(v); return 0; }
    return v;
}

const char *fat_label(fatvol_t *v) { return v->label; }
blockdev_t *fat_device(fatvol_t *v) { return v->dev; }
UINT32 fat_cluster_bytes(fatvol_t *v) { return v->spc * 512; }
int fat_type(fatvol_t *v) { return v->type; }

static UINT64 cl_lba(fatvol_t *v, UINT32 c) { return (UINT64)v->data_lba + (UINT64)(c - 2) * v->spc; }

static int read_cached(fatvol_t *v, UINT8 *buf, UINT64 *cur_lba, int *ok, UINT64 lba) {
    if (*ok && *cur_lba == lba) return 0;
    if (blk_read(v->dev, lba, 1, buf) != 0) { *ok = 0; return -1; }
    *cur_lba = lba; *ok = 1;
    return 0;
}

static void drop_caches(fatvol_t *v) { v->fsec_ok = v->dsec_ok = v->xsec_ok = 0; }

static int is_eoc(fatvol_t *v, UINT32 c) { return v->type == 32 ? c >= 0x0FFFFFF8u : c >= 0xFFF8u; }
static UINT32 eoc_value(fatvol_t *v) { return v->type == 32 ? 0x0FFFFFFFu : 0xFFFFu; }

// Kume zincirindeki sonraki kume (EOC degeri veya >= 0xFFFFFFF0: hata)
static UINT32 fat_next(fatvol_t *v, UINT32 c) {
    UINT32 off = v->type == 32 ? c * 4 : c * 2;
    if (read_cached(v, v->fsec, &v->fsec_lba, &v->fsec_ok, v->reserved + off / 512) != 0) return 0xFFFFFFFFu;
    const UINT8 *p = v->fsec + off % 512;
    return v->type == 32 ? (u32(p) & 0x0FFFFFFFu) : u16(p);
}

// ------------------------------------------------------------ FAT yazma yardimcilari

// FAT girdisini TUM FAT kopyalarinda gunceller.
static int set_fat(fatvol_t *v, UINT32 c, UINT32 val) {
    UINT32 off = v->type == 32 ? c * 4 : c * 2;
    UINT32 rel = off / 512, o = off % 512;
    UINT8 tmp[512];
    for (UINT32 copy = 0; copy < v->nfats; copy++) {
        UINT64 lba = (UINT64)v->reserved + (UINT64)copy * v->fatsz + rel;
        if (blk_read(v->dev, lba, 1, tmp) != 0) return -5;
        if (v->type == 32) put32(tmp + o, (u32(tmp + o) & 0xF0000000u) | (val & 0x0FFFFFFFu));
        else put16(tmp + o, (UINT16)val);
        if (blk_write(v->dev, lba, 1, tmp) != 0) return -5;
    }
    v->fsec_ok = 0;
    return 0;
}

static UINT32 alloc_cluster(fatvol_t *v, UINT32 prev) {
    UINT32 total = v->clusters + 2;
    for (UINT32 n = 0; n < v->clusters; n++) {
        UINT32 c = v->free_hint + n;
        if (c >= total) c = 2 + (c - total);
        if (fat_next(v, c) != 0) continue;
        if (set_fat(v, c, eoc_value(v)) != 0) return 0;
        if (prev >= 2 && set_fat(v, prev, c) != 0) return 0;
        v->free_hint = c + 1;
        if (v->free_count != 0xFFFFFFFFu && v->free_count > 0) { v->free_count--; v->fsinfo_dirty = 1; }
        v->next_free = c; 
        return c;
    }
    return 0;
}

static void free_chain(fatvol_t *v, UINT32 c) {
    while (c >= 2 && c < 0xFFFFFFF0u && !is_eoc(v, c)) {
        UINT32 next = fat_next(v, c);
        if (set_fat(v, c, 0) != 0) return;
        if (v->free_count != 0xFFFFFFFFu) { v->free_count++; v->fsinfo_dirty = 1; }
        if (c < v->free_hint) v->free_hint = c;
        c = next;
    }
}

static int zero_cluster(fatvol_t *v, UINT32 c) {
    UINT8 z[512];
    memset(z, 0, sizeof z);
    for (UINT32 s = 0; s < v->spc; s++)
        if (blk_write(v->dev, cl_lba(v, c) + s, 1, z) != 0) return -5;
    return 0;
}

void fat_sync(fatvol_t *v) {
    if (!v->fsinfo_dirty || !v->fsinfo_lba) return;
    UINT8 f[512];
    if (blk_read(v->dev, v->fsinfo_lba, 1, f) != 0) return;
    put32(f + 488, v->free_count);
    put32(f + 492, v->next_free);
    blk_write(v->dev, v->fsinfo_lba, 1, f);
    v->fsinfo_dirty = 0;
}

int fat_space(fatvol_t *v, UINT64 *total, UINT64 *freec) {
    UINT64 f = 0;
    for (UINT32 c = 2; c < v->clusters + 2; c++) if (fat_next(v, c) == 0) f++;
    *total = v->clusters; *freec = f;
    return 0;
}

static void now_fat(UINT16 *date, UINT16 *time) {
    rtc_time_t t;
    rtc_get(&t);
    int y = t.year < 1980 ? 1980 : t.year;
    *date = (UINT16)(((y - 1980) << 9) | (t.month << 5) | t.day);
    *time = (UINT16)((t.hour << 11) | (t.min << 5) | (t.sec / 2));
}

// ---------------------------------------------------------------- dizin okuma

// Dizin girdisinin diskteki sektoru. dircl==0: kok dizin. 0 = tamam, 1 = dizin zinciri bitti, -1 = hata
static int dir_locate(fatvol_t *v, UINT32 dircl, UINT32 idx, UINT64 *lba) {
    if (dircl == 0 && v->type == 16) {
        if (idx >= v->root_entries) return 1;
        *lba = v->root_lba + idx / 16;
        return 0;
    }
    UINT32 c = dircl ? dircl : v->root_cl;
    UINT32 per_cl = v->spc * 16, step = idx / per_cl;
    for (UINT32 i = 0; i < step; i++) {
        c = fat_next(v, c);
        if (c >= 0xFFFFFFF0u) return -1;
        if (is_eoc(v, c) || c < 2) return 1;
    }
    *lba = cl_lba(v, c) + (idx % per_cl) / 16;
    return 0;
}

static int dir_get(fatvol_t *v, UINT32 dircl, UINT32 idx, UINT8 out[32]) {
    UINT64 lba;
    int r = dir_locate(v, dircl, idx, &lba);
    if (r != 0) return r;
    if (read_cached(v, v->dsec, &v->dsec_lba, &v->dsec_ok, lba) != 0) return -1;
    memcpy(out, v->dsec + (idx % 16) * 32, 32);
    return 0;
}

// Dizin girdisini diske yazar (sektoru oku-degistir-yaz).
static int dir_put(fatvol_t *v, UINT32 dircl, UINT32 idx, const UINT8 e[32]) {
    UINT64 lba;
    if (dir_locate(v, dircl, idx, &lba) != 0) return -5;
    UINT8 tmp[512];
    if (blk_read(v->dev, lba, 1, tmp) != 0) return -5;
    memcpy(tmp + (idx % 16) * 32, e, 32);
    if (blk_write(v->dev, lba, 1, tmp) != 0) return -5;
    v->dsec_ok = 0;
    return 0;
}

static UINT8 sfn_checksum(const UINT8 *e) {
    UINT8 sum = 0;
    for (int i = 0; i < 11; i++) sum = (UINT8)(((sum & 1) << 7) + (sum >> 1) + e[i]);
    return sum;
}

static const UINT8 g_lfn_offs[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };

// Sonraki gecerli girdiyi (LFN birlestirerek) okur. 0 = girdi, 1 = bitti, -1 = hata
static int dir_next(fatvol_t *v, UINT32 dircl, UINT32 *cur, fat_entry_t *out) {
    char lfn[260];
    int lfn_ok = 0;
    UINT32 lfn_start = 0;
    UINT8 csum = 0;
    memset(lfn, 0, sizeof lfn);
    for (;;) {
        UINT8 e[32];
        int r = dir_get(v, dircl, *cur, e);
        if (r != 0) return r;
        UINT32 here = (*cur)++;
        if (e[0] == 0x00) return 1;
        if (e[0] == 0xE5) { lfn_ok = 0; continue; }
        if (e[11] == 0x0F) {                                          // uzun ad parcasi
            int seq = e[0] & 0x1F;
            if (e[0] & 0x40) { memset(lfn, 0, sizeof lfn); lfn_ok = 1; csum = e[13]; lfn_start = here; }
            if (!lfn_ok || seq < 1 || seq > 20) { lfn_ok = 0; continue; }
            for (int k = 0; k < 13; k++) {
                UINT16 ch = u16(e + g_lfn_offs[k]);
                if (ch == 0 || ch == 0xFFFF) break;
                lfn[(seq - 1) * 13 + k] = ch < 128 ? (char)ch : '?';
            }
            continue;
        }
        if (e[11] & 0x08) { lfn_ok = 0; continue; }                   // birim etiketi
        out->lfn_idx = here;
        if (lfn_ok && sfn_checksum(e) == csum && lfn[0]) {
            size_t n = strlen(lfn); if (n > 255) n = 255;
            memcpy(out->name, lfn, n); out->name[n] = 0;
            out->lfn_idx = lfn_start;
        } else {
            int n = 0;
            for (int i = 0; i < 8 && e[i] != ' '; i++) { char c = (char)e[i]; if (i == 0 && (UINT8)c == 0x05) c = (char)0xE5; if ((e[12] & 0x08) && c >= 'A' && c <= 'Z') c += 32; out->name[n++] = c; }
            if (e[8] != ' ') {
                out->name[n++] = '.';
                for (int i = 8; i < 11 && e[i] != ' '; i++) { char c = (char)e[i]; if ((e[12] & 0x10) && c >= 'A' && c <= 'Z') c += 32; out->name[n++] = c; }
            }
            out->name[n] = 0;
        }
        lfn_ok = 0;
        if (!strcmp(out->name, ".") || !strcmp(out->name, "..")) continue;
        out->cluster = ((UINT32)u16(e + 20) << 16) | u16(e + 26);
        out->size = u32(e + 28);
        out->wtime = u16(e + 22); out->wdate = u16(e + 24);
        out->attr = e[11];
        out->is_dir = (e[11] & 0x10) != 0;
        out->dir_cl = dircl;
        out->idx = here;
        return 0;
    }
}

// ----------------------------------------------------------------- yol cozumleme

static int lookup(fatvol_t *v, const char *path, fat_entry_t *out) {
    memset(out, 0, sizeof *out);
    out->is_dir = 1;                                                  // "/" : kok
    out->attr = 0x10;
    UINT32 dircl = 0;
    const char *p = path;
    while (*p == '/') p++;
    if (!*p) return 0;
    while (*p) {
        char comp[256];
        int n = 0;
        while (*p && *p != '/' && n < 255) comp[n++] = *p++;
        comp[n] = 0;
        while (*p == '/') p++;
        if (!out->is_dir) return -1;                                  // dosyanin altina inilemez
        UINT32 cur = 0;
        fat_entry_t e;
        int found = 0;
        while (dir_next(v, dircl, &cur, &e) == 0)
            if (!strcasecmp(e.name, comp)) { found = 1; break; }
        if (!found) return -1;
        *out = e;
        dircl = e.cluster;
    }
    return 0;
}

int fat_stat(fatvol_t *v, const char *path, fat_entry_t *out) { return lookup(v, path, out); }

int fat_readdir(fatvol_t *v, const char *path, int index, fat_entry_t *out) {
    fat_entry_t d;
    if (lookup(v, path, &d) != 0 || !d.is_dir) return -1;
    UINT32 cur = 0;
    for (int i = 0;; i++) {
        int r = dir_next(v, d.cluster, &cur, out);
        if (r != 0) return r;
        if (i == index) return 0;
    }
}

// ------------------------------------------------------------------ dosya okuma

long long fat_read(fatvol_t *v, const fat_entry_t *f, UINT64 off, void *buf, UINT64 len) {
    if (f->is_dir) return -1;
    if (off >= f->size) return 0;
    if (off + len > f->size) len = f->size - off;
    if (f->cluster < 2) return 0;

    UINT32 csz = v->spc * 512;
    UINT32 c = f->cluster;
    for (UINT64 i = 0; i < off / csz; i++) {
        c = fat_next(v, c);
        if (c < 2 || c >= 0xFFFFFFF0u || is_eoc(v, c)) return -1;
    }
    UINT8 *dst = buf;
    UINT64 done = 0;
    UINT64 pos = off % csz;
    while (done < len) {
        UINT32 sec = (UINT32)(pos / 512), so = (UINT32)(pos % 512);
        UINT64 lba = cl_lba(v, c) + sec;
        UINT64 remain = len - done;
        UINT32 secs_left = v->spc - sec;
        if (so == 0 && remain >= 512) {                               // hizali: dogrudan hedefe oku
            UINT32 n = (UINT32)(remain / 512);
            if (n > secs_left) n = secs_left;
            if (n > 64) n = 64;
            if (blk_read(v->dev, lba, n, dst + done) != 0) return -1;
            done += (UINT64)n * 512; pos += (UINT64)n * 512;
        } else {                                                      // kismi sektor
            if (read_cached(v, v->xsec, &v->xsec_lba, &v->xsec_ok, lba) != 0) return -1;
            UINT64 n = 512 - so; if (n > remain) n = remain;
            memcpy(dst + done, v->xsec + so, (size_t)n);
            done += n; pos += n;
        }
        if (pos >= csz && done < len) {
            c = fat_next(v, c);
            if (c < 2 || c >= 0xFFFFFFF0u || is_eoc(v, c)) return -1;
            pos = 0;
        }
    }
    return (long long)done;
}

// ================================================================== YAZMA

// "/a/b/c" -> parent="/a/b", name="c"
static int split_path(const char *path, char *parent, UINT32 pcap, char *name, UINT32 ncap) {
    size_t n = strlen(path);
    while (n > 1 && path[n - 1] == '/') n--;
    if (n == 0 || (n == 1 && path[0] == '/')) return -1;              // kok
    size_t i = n;
    while (i > 0 && path[i - 1] != '/') i--;
    size_t nl = n - i;
    if (nl == 0 || nl >= ncap) return -1;
    memcpy(name, path + i, nl); name[nl] = 0;
    size_t pl = i > 1 ? i - 1 : 1;
    if (pl >= pcap) return -1;
    if (i > 1) { memcpy(parent, path, pl); parent[pl] = 0; } else { parent[0] = '/'; parent[1] = 0; }
    return 0;
}

static int name_ok(const char *n) {
    size_t len = strlen(n);
    if (len == 0 || len > 255 || !strcmp(n, ".") || !strcmp(n, "..")) return 0;
    if (n[len - 1] == ' ' || n[len - 1] == '.') return 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)n[i];
        if (c < 32 || c >= 127 || strchr("\\/:*?\"<>|", c)) return 0;
    }
    return 1;
}

static int sfn_char_ok(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || (c && strchr("!#$%&'()-@^_`{}~", c));
}

static int sfn_exists(fatvol_t *v, UINT32 dircl, const UINT8 sfn[11]) {
    for (UINT32 idx = 0;; idx++) {
        UINT8 e[32];
        if (dir_get(v, dircl, idx, e) != 0 || e[0] == 0x00) return 0;
        if (e[0] == 0xE5 || e[11] == 0x0F) continue;
        if (!memcmp(e, sfn, 11)) return 1;
    }
}

// Ad -> 8.3 kisa ad (+ kucuk harf bayraklari). Gerekirse LFN ve "~N" kuyrugu uretir. needs_lfn doner.
static void make_sfn(fatvol_t *v, UINT32 dircl, const char *name, UINT8 sfn[11], UINT8 *flags, int *needs_lfn) {
    size_t len = strlen(name);
    const char *dot = strrchr(name, '.');
    size_t base_end = dot && dot != name ? (size_t)(dot - name) : len;
    const char *ext = dot && dot != name ? dot + 1 : "";
    size_t ext_len = strlen(ext);

    char base[256]; int bl = 0, lossy = 0, b_up = 0, b_lo = 0, e_up = 0, e_lo = 0;
    for (size_t i = 0; i < base_end; i++) {
        char c = name[i];
        if (c == ' ' || c == '.') { lossy = 1; continue; }
        if (c >= 'a' && c <= 'z') { b_lo = 1; c -= 32; } else if (c >= 'A' && c <= 'Z') b_up = 1;
        if (!sfn_char_ok(c)) { c = '_'; lossy = 1; }
        base[bl++] = c;
    }
    char ex[256]; int el = 0;
    for (size_t i = 0; i < ext_len; i++) {
        char c = ext[i];
        if (c == ' ') { lossy = 1; continue; }
        if (c >= 'a' && c <= 'z') { e_lo = 1; c -= 32; } else if (c >= 'A' && c <= 'Z') e_up = 1;
        if (!sfn_char_ok(c)) { c = '_'; lossy = 1; }
        ex[el++] = c;
    }
    int mixed = (b_lo && b_up) || (e_lo && e_up);
    int too_long = bl > 8 || el > 3 || bl == 0 || (dot == name);          // gizli ad (".x") da LFN ister
    *needs_lfn = lossy || mixed || too_long;
    *flags = 0;
    if (!*needs_lfn) {
        if (b_lo && !b_up) *flags |= 0x08;
        if (e_lo && !e_up) *flags |= 0x10;
    }
    if (bl == 0) { base[0] = '_'; bl = 1; }

    memset(sfn, ' ', 11);
    if (!*needs_lfn) {
        memcpy(sfn, base, (size_t)bl);
        memcpy(sfn + 8, ex, (size_t)el);
        return;
    }
    int keep = bl > 6 ? 6 : bl;
    for (int n = 1; n < 100000; n++) {
        char tail[8];
        int tl = 0, t = n;
        char rev[8]; int rl = 0;
        while (t) { rev[rl++] = (char)('0' + t % 10); t /= 10; }
        tail[tl++] = '~';
        while (rl) tail[tl++] = rev[--rl];
        memset(sfn, ' ', 11);
        int kb = keep; if (kb + tl > 8) kb = 8 - tl;
        memcpy(sfn, base, (size_t)kb);
        memcpy(sfn + kb, tail, (size_t)tl);
        memcpy(sfn + 8, ex, (size_t)(el > 3 ? 3 : el));
        if (!sfn_exists(v, dircl, sfn)) return;
    }
}

// dircl dizininde n ardisik bos girdi bulur (gerekirse dizini genisletir). 0 = tamam
static int dir_find_slots(fatvol_t *v, UINT32 dircl, UINT32 n, UINT32 *first) {
    UINT32 run = 0, start = 0;
    for (UINT32 idx = 0;; idx++) {
        UINT8 e[32];
        int r = dir_get(v, dircl, idx, e);
        if (r < 0) return -5;
        if (r == 1) {                                                 // zincir bitti: genislet
            if (dircl == 0 && v->type == 16) return -3;               // FAT16 koku sabit boyutlu
            UINT32 last = dircl ? dircl : v->root_cl;
            for (;;) { UINT32 nx = fat_next(v, last); if (nx < 2 || nx >= 0xFFFFFFF0u || is_eoc(v, nx)) break; last = nx; }
            UINT32 nc = alloc_cluster(v, last);
            if (!nc) return -4;
            if (zero_cluster(v, nc) != 0) return -5;
            idx--;                                                    // ayni indeksi yeniden dene
            continue;
        }
        if (e[0] == 0x00 || e[0] == 0xE5) {
            if (run == 0) start = idx;
            if (++run == n) { *first = start; return 0; }
        } else run = 0;
    }
}

// Yeni dizin girdisi (LFN gerekirse onunla birlikte) olusturur.
static int create_entry(fatvol_t *v, UINT32 dircl, const char *name, UINT8 attr, UINT32 cluster, UINT32 size,
                        UINT16 wdate, UINT16 wtime) {
    if (!name_ok(name)) return -1;
    UINT32 cur = 0;
    fat_entry_t ex;
    while (dir_next(v, dircl, &cur, &ex) == 0)
        if (!strcasecmp(ex.name, name)) return -2;

    UINT8 sfn[11], flags;
    int lfn;
    make_sfn(v, dircl, name, sfn, &flags, &lfn);
    UINT32 len = (UINT32)strlen(name);
    UINT32 nlfn = lfn ? (len + 12) / 13 : 0;
    UINT32 first;
    int r = dir_find_slots(v, dircl, nlfn + 1, &first);
    if (r != 0) return r;

    UINT8 csum = sfn_checksum(sfn);
    for (UINT32 i = 0; i < nlfn; i++) {
        UINT32 seq = nlfn - i;
        UINT8 e[32];
        memset(e, 0, sizeof e);
        e[0] = (UINT8)(seq | (i == 0 ? 0x40 : 0));
        e[11] = 0x0F; e[13] = csum;
        for (UINT32 k = 0; k < 13; k++) {
            UINT32 ci = (seq - 1) * 13 + k;
            UINT16 ch = ci < len ? (UINT16)(unsigned char)name[ci] : (ci == len ? 0x0000 : 0xFFFF);
            put16(e + g_lfn_offs[k], ch);
        }
        if (dir_put(v, dircl, first + i, e) != 0) return -5;
    }
    UINT8 e[32];
    memset(e, 0, sizeof e);
    memcpy(e, sfn, 11);
    e[11] = attr; e[12] = flags;
    put16(e + 14, wtime); put16(e + 16, wdate); put16(e + 18, wdate);
    put16(e + 20, (UINT16)(cluster >> 16)); put16(e + 22, wtime); put16(e + 24, wdate); put16(e + 26, (UINT16)cluster);
    put32(e + 28, size);
    return dir_put(v, dircl, first + nlfn, e) == 0 ? 0 : -5;
}

static int dir_update(fatvol_t *v, const fat_entry_t *f, UINT32 cluster, UINT32 size) {
    UINT8 e[32];
    if (dir_get(v, f->dir_cl, f->idx, e) != 0) return -5;
    UINT16 d, t;
    now_fat(&d, &t);
    put16(e + 20, (UINT16)(cluster >> 16)); put16(e + 26, (UINT16)cluster);
    put32(e + 28, size);
    put16(e + 22, t); put16(e + 24, d); put16(e + 18, d);
    e[11] |= 0x20;                                                    // arsiv biti
    return dir_put(v, f->dir_cl, f->idx, e);
}

int fat_create(fatvol_t *v, const char *path, int is_dir) {
    char parent[256], name[256];
    if (split_path(path, parent, sizeof parent, name, sizeof name) != 0) return -1;
    fat_entry_t p;
    if (lookup(v, parent, &p) != 0 || !p.is_dir) return -1;
    UINT16 d, t;
    now_fat(&d, &t);
    int r;
    if (!is_dir) {
        r = create_entry(v, p.cluster, name, 0x20, 0, 0, d, t);
    } else {
        UINT32 nc = alloc_cluster(v, 0);
        if (!nc) return -4;
        if (zero_cluster(v, nc) != 0) { free_chain(v, nc); return -5; }
        UINT8 e[32];
        for (int k = 0; k < 2; k++) {                                 // "." ve ".."
            memset(e, 0, sizeof e); memset(e, ' ', 11);
            e[0] = '.'; if (k) e[1] = '.';
            e[11] = 0x10;
            UINT32 cl = k ? p.cluster : nc;                           // kokun ust kumesi 0'dir
            put16(e + 20, (UINT16)(cl >> 16)); put16(e + 26, (UINT16)cl);
            put16(e + 22, t); put16(e + 24, d);
            if (dir_put(v, nc, (UINT32)k, e) != 0) { free_chain(v, nc); return -5; }
        }
        r = create_entry(v, p.cluster, name, 0x10, nc, 0, d, t);
        if (r != 0) free_chain(v, nc);
    }
    drop_caches(v);
    return r;
}

long long fat_write(fatvol_t *v, const char *path, UINT64 off, const void *buf, UINT64 len) {
    fat_entry_t f;
    if (lookup(v, path, &f) != 0 || f.is_dir) return -1;
    if (off > f.size) return -1;                                      // bosluklu (sparse) yazma yok
    if (len == 0) return 0;
    UINT64 new_size = off + len > f.size ? off + len : f.size;
    if (new_size > 0xFFFFFFFFULL) return -4;

    UINT32 csz = v->spc * 512;
    UINT32 first = f.cluster, orig_first = f.cluster;
    if (first < 2) { first = alloc_cluster(v, 0); if (!first) return -4; }

    UINT32 c = first;
    for (UINT64 i = 0; i < off / csz; i++) {                          // yazma noktasindaki kumeye ilerle
        UINT32 n = fat_next(v, c);
        if (n < 2 || n >= 0xFFFFFFF0u || is_eoc(v, n)) { n = alloc_cluster(v, c); if (!n) goto nospace; }
        c = n;
    }

    {
    const UINT8 *src = buf;
    UINT64 done = 0, pos = off % csz;
    while (done < len) {
        UINT32 sec = (UINT32)(pos / 512), so = (UINT32)(pos % 512);
        UINT64 lba = cl_lba(v, c) + sec;
        UINT64 remain = len - done;
        UINT32 secs_left = v->spc - sec;
        if (so == 0 && remain >= 512) {
            UINT32 n = (UINT32)(remain / 512);
            if (n > secs_left) n = secs_left;
            if (n > 64) n = 64;
            if (blk_write(v->dev, lba, n, src + done) != 0) { drop_caches(v); return -5; }
            done += (UINT64)n * 512; pos += (UINT64)n * 512;
        } else {
            UINT8 tmp[512];
            if (blk_read(v->dev, lba, 1, tmp) != 0) { drop_caches(v); return -5; }
            UINT64 n = 512 - so; if (n > remain) n = remain;
            memcpy(tmp + so, src + done, (size_t)n);
            if (blk_write(v->dev, lba, 1, tmp) != 0) { drop_caches(v); return -5; }
            done += n; pos += n;
        }
        if (pos >= csz && done < len) {
            UINT32 n = fat_next(v, c);
            if (n < 2 || n >= 0xFFFFFFF0u || is_eoc(v, n)) { n = alloc_cluster(v, c); if (!n) goto nospace_partial; }
            c = n; pos = 0;
        }
    }
    drop_caches(v);
    if (new_size != f.size || first != orig_first) {
        if (dir_update(v, &f, first, (UINT32)new_size) != 0) return -5;
    }
    return (long long)done;

nospace_partial:
    drop_caches(v);
    {
        UINT64 total = off + done;                                    // yazilabilen kismi kaydet
        if (total > f.size && dir_update(v, &f, first, (UINT32)total) != 0) return -5;
        return done ? (long long)done : -4;
    }
    }

nospace:
    if (orig_first < 2) free_chain(v, first);
    drop_caches(v);
    return -4;
}

int fat_truncate(fatvol_t *v, const char *path, UINT64 size) {
    fat_entry_t f;
    if (lookup(v, path, &f) != 0 || f.is_dir) return -1;
    if (size > f.size) return -1;
    if (size == f.size) return 0;
    UINT32 csz = v->spc * 512;
    if (size == 0) {
        free_chain(v, f.cluster);
        drop_caches(v);
        return dir_update(v, &f, 0, 0);
    }
    UINT32 keep = (UINT32)((size + csz - 1) / csz), c = f.cluster;
    for (UINT32 i = 1; i < keep; i++) {
        c = fat_next(v, c);
        if (c < 2 || c >= 0xFFFFFFF0u || is_eoc(v, c)) return -5;
    }
    UINT32 tail = fat_next(v, c);
    if (set_fat(v, c, eoc_value(v)) != 0) return -5;
    free_chain(v, tail);
    drop_caches(v);
    return dir_update(v, &f, f.cluster, (UINT32)size);
}

static int dir_is_empty(fatvol_t *v, UINT32 cluster) {
    UINT32 cur = 0;
    fat_entry_t e;
    return dir_next(v, cluster, &cur, &e) != 0;                       // girdi yoksa (1) bos
}

static int mark_deleted(fatvol_t *v, const fat_entry_t *f) {
    for (UINT32 i = f->lfn_idx; i <= f->idx; i++) {
        UINT8 e[32];
        if (dir_get(v, f->dir_cl, i, e) != 0) return -5;
        e[0] = 0xE5;
        if (dir_put(v, f->dir_cl, i, e) != 0) return -5;
    }
    return 0;
}

int fat_remove(fatvol_t *v, const char *path) {
    fat_entry_t f;
    if (lookup(v, path, &f) != 0) return -1;
    if (f.dir_cl == 0 && f.idx == 0 && f.cluster == 0 && f.is_dir && f.name[0] == 0) return -1;   // kok
    if (f.is_dir && !dir_is_empty(v, f.cluster)) return -3;
    int r = mark_deleted(v, &f);
    if (r == 0) free_chain(v, f.cluster);
    drop_caches(v);
    return r;
}

int fat_rename(fatvol_t *v, const char *from, const char *to) {
    fat_entry_t f, ex;
    if (lookup(v, from, &f) != 0 || f.name[0] == 0) return -1;
    if (lookup(v, to, &ex) == 0) return -2;
    char parent[256], name[256];
    if (split_path(to, parent, sizeof parent, name, sizeof name) != 0) return -1;
    fat_entry_t p;
    if (lookup(v, parent, &p) != 0 || !p.is_dir) return -1;
    if (f.is_dir) {                                                   // dizini kendi altina tasima
        size_t fl = strlen(from);
        if (!strncasecmp(to, from, fl) && (to[fl] == '/' || to[fl] == 0)) return -1;
    }
    int r = create_entry(v, p.cluster, name, f.attr, f.cluster, f.size, f.wdate, f.wtime);
    if (r != 0) { drop_caches(v); return r; }
    r = mark_deleted(v, &f);                                          // eski girdi silinir, kume zinciri KORUNUR
    if (r == 0 && f.is_dir && p.cluster != f.dir_cl) {                // ".." girdisini yeni ust dizine cevir
        UINT8 e[32];
        if (dir_get(v, f.cluster, 1, e) == 0) {
            put16(e + 20, (UINT16)(p.cluster >> 16)); put16(e + 26, (UINT16)p.cluster);
            dir_put(v, f.cluster, 1, e);
        }
    }
    drop_caches(v);
    return r;
}

// DEOS -- acik dosya nesneleri: konsol, dosya, dizin. Syscall katmani bunlari fd tablosu uzerinden kullanir.
#include "types.h"
#include "file.h"
#include "fs.h"
#include "heap.h"
#include "console.h"
#include "thread.h"
#include "string.h"

static file_t g_con[3];
static int g_con_init;

int fs_err_to_errno(int e) {
    switch (e) {
    case -1: return ENOENT_RET;
    case -2: return EEXIST_RET;
    case -3: return ENOTEMPTY_RET;
    case -4: return ENOSPC_RET;
    case -5: return EIO_RET;
    default: return e < 0 ? e : EIO_RET;
    }
}

file_t *file_console(int fd) {
    if (!g_con_init) {
        for (int i = 0; i < 3; i++) { g_con[i].kind = i == 0 ? FK_CON_IN : FK_CON_OUT; g_con[i].refs = 1; }    // sistem bir referans tutar
        g_con_init = 1;
    }
    file_t *f = &g_con[fd < 0 || fd > 2 ? 1 : fd];
    f->refs++;
    return f;
}

void file_ref(file_t *f) { if (f) f->refs++; }

void fileobj_close(file_t *f);
long long fileobj_read(file_t *f, void *buf, UINT64 n);
long long fileobj_write(file_t *f, const void *buf, UINT64 n);

void file_unref(file_t *f) {
    if (!f) return;
    if (f->kind == FK_CON_IN || f->kind == FK_CON_OUT) { if (f->refs > 1) f->refs--; return; }    // statik konsol nesneleri
    if (--f->refs <= 0) {
        if (f->kind == FK_PIPE_R || f->kind == FK_PIPE_W || f->kind == FK_SOCK) fileobj_close(f);
        kfree(f);
    }
}

file_t *file_open(const char *path, int flags, int *err) {
    fs_dirent_t e;
    int exists = fs_stat(path, &e) == 0;
    int acc = flags & 3;
    *err = 0;
    int kind = FK_FILE;
    if (flags & O_DIRECTORY) {
        if (!exists) { *err = ENOENT_RET; return 0; }
        if (!e.is_dir) { *err = ENOTDIR_RET; return 0; }
        kind = FK_DIR;
    } else if (!exists) {
        if (!(flags & O_CREAT)) { *err = ENOENT_RET; return 0; }
        int r = fs_create(path, 0);
        if (r) { *err = fs_err_to_errno(r); return 0; }
    } else {
        if (e.is_dir) { *err = EISDIR_RET; return 0; }
        if ((flags & O_TRUNC) && acc != O_RDONLY) {
            int r = fs_write_file(path, "", 0);
            if (r) { *err = fs_err_to_errno(r); return 0; }
        }
    }
    file_t *f = kcalloc(1, sizeof *f);
    if (!f) { *err = ENOSPC_RET; return 0; }
    f->refs = 1; f->kind = kind; f->flags = flags;
    size_t n = strlen(path); if (n > 159) n = 159;
    memcpy(f->path, path, n); f->path[n] = 0;
    return f;
}

long long file_read(file_t *f, void *buf, UINT64 n) {
    if (n == 0) return 0;
    if (f->kind == FK_CON_IN) {
        UINT8 *p = buf;
        int c;
        while ((c = console_getchar()) < 0) thread_sleep_ms(10);             // en az bir karakter bekle
        UINT64 got = 0;
        p[got++] = (UINT8)c;
        while (got < n && (c = console_getchar()) >= 0) p[got++] = (UINT8)c;  // varsa gerisini bloklamadan al
        return (long long)got;
    }
    if (f->kind == FK_PIPE_R || f->kind == FK_SOCK) return fileobj_read(f, buf, n);
    if (f->kind != FK_FILE) return f->kind == FK_DIR ? EISDIR_RET : EBADF_RET;
    if ((f->flags & 3) == O_WRONLY) return EBADF_RET;
    long long r = fs_read(f->path, f->pos, buf, n);
    if (r < 0) return EIO_RET;
    f->pos += (UINT64)r;
    return r;
}

long long file_write(file_t *f, const void *buf, UINT64 n) {
    if (f->kind == FK_CON_OUT) { const char *p = buf; for (UINT64 i = 0; i < n; i++) console_putc(p[i]); return (long long)n; }
    if (f->kind == FK_PIPE_W || f->kind == FK_SOCK) return fileobj_write(f, buf, n);
    if (f->kind != FK_FILE) return f->kind == FK_DIR ? EISDIR_RET : EBADF_RET;
    if ((f->flags & 3) == O_RDONLY) return EBADF_RET;
    if (n == 0) return 0;
    fs_dirent_t e;
    if (fs_stat(f->path, &e) != 0) return ENOENT_RET;
    if (f->flags & O_APPEND) f->pos = e.size;
    if (f->pos > e.size) return EINVAL_RET;                                  // bosluklu (sparse) yazma desteklenmiyor
    long long w = fs_write(f->path, f->pos, buf, n);
    if (w < 0) return fs_err_to_errno((int)w);
    f->pos += (UINT64)w;
    return w;
}

long long file_seek(file_t *f, long long off, int whence) {
    if (f->kind != FK_FILE) return EBADF_RET;
    fs_dirent_t e;
    if (fs_stat(f->path, &e) != 0) return ENOENT_RET;
    long long base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? (long long)f->pos : whence == SEEK_END ? (long long)e.size : -1;
    if (base < 0 || base + off < 0) return EINVAL_RET;
    f->pos = (UINT64)(base + off);
    return (long long)f->pos;
}

int file_readdir(file_t *f, udirent_t *out) {
    if (f->kind != FK_DIR) return ENOTDIR_RET;
    fs_dirent_t e;
    int r = fs_readdir(f->path, (int)f->dir_idx, &e);
    if (r == 1) return 0;
    if (r < 0) return EIO_RET;
    f->dir_idx++;
    memset(out, 0, sizeof *out);
    size_t n = strlen(e.name); if (n > 63) n = 63;
    memcpy(out->name, e.name, n);
    out->size = e.size; out->is_dir = e.is_dir;
    return 1;
}

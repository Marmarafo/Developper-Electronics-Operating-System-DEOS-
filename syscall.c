// DEOS -- sistem cagrilari (syscall). Kullanici programlari "syscall" talimatiyla kernel'e girer
// (usermode.S -> syscall_dispatch). Kullanici gostericileri ASLA dogrudan kullanilmaz: sayfa tablosu
// uzerinden dogrulanip fiziksel adres uzerinden kopyalanir.
#include "types.h"
#include "syscall.h"
#include "syscall_abi.h"
#include "cpu.h"
#include "console.h"
#include "timer.h"
#include "thread.h"
#include "process.h"
#include "paging.h"
#include "string.h"
#include "kprintf.h"
#include "fs.h"
#include "file.h"
#include "heap.h"

#define MSR_EFER   0xC0000080u
#define MSR_STAR   0xC0000081u
#define MSR_LSTAR  0xC0000082u
#define MSR_SFMASK 0xC0000084u

extern void syscall_entry(void);        // usermode.S

UINT64 g_syscall_kstack;                // usermode.S kullanir
UINT64 g_user_rsp_tmp;                  // usermode.S kullanir
static int g_smep;

void syscall_set_kstack(UINT64 kstack_top) { g_syscall_kstack = kstack_top; }
int  syscall_smep_enabled(void) { return g_smep; }

void syscall_init(void) {
    cpu_wrmsr(MSR_EFER, cpu_rdmsr(MSR_EFER) | 1);                        // SCE: syscall/sysret acik
    cpu_wrmsr(MSR_STAR, (0x10ULL << 48) | (0x08ULL << 32));              // sysret: CS=0x10+16, SS=0x10+8 ; syscall: CS=0x08
    cpu_wrmsr(MSR_LSTAR, (UINT64)(UINTN)syscall_entry);
    cpu_wrmsr(MSR_SFMASK, 0x200 | 0x400 | 0x100 | 0x40000);              // girisde IF, DF, TF, AC temizlenir

    // SMEP: kernel, kullanici sayfalarinda KOD calistiramaz (CPUID.7.0:EBX[7])
    UINT32 maxleaf, b, c, d;
    __asm__ __volatile__ ("cpuid" : "=a"(maxleaf), "=b"(b), "=c"(c), "=d"(d) : "a"(0), "c"(0));
    if (maxleaf >= 7) {
        UINT32 a7;
        __asm__ __volatile__ ("cpuid" : "=a"(a7), "=b"(b), "=c"(c), "=d"(d) : "a"(7), "c"(0));
        if ((b >> 7) & 1) {
            UINT64 cr4;
            __asm__ __volatile__ ("mov %%cr4, %0" : "=r"(cr4));
            cr4 |= (1ULL << 20);
            __asm__ __volatile__ ("mov %0, %%cr4" : : "r"(cr4) : "memory");
            g_smep = 1;
        }
    }
}

// ------------------------------------------------- kullanici bellegine guvenli erisim

static UINT64 current_cr3(void) {
    UINT64 cr3;
    __asm__ __volatile__ ("mov %%cr3, %0" : "=r"(cr3));
    return cr3;
}

// Kullanici adresiyle fiziksel sayfa arasinda kopyalar; her sayfa U (ve yazma icin W) izni icin dogrulanir.
static int user_copy(void *kbuf, UINT64 uaddr, UINT64 n, int to_user) {
    if (uaddr < USER_BASE || uaddr + n < uaddr || uaddr + n > USER_TOP) return -1;
    UINT64 cr3 = current_cr3();
    UINT8 *k = kbuf;
    while (n) {
        UINT64 phys, flags;
        UINT32 shift;
        if (!paging_query_in(cr3, uaddr, &phys, &flags, &shift)) return -1;
        if (!(flags & PTE_U)) return -1;
        if (to_user && !(flags & PTE_W)) return -1;
        UINT64 chunk = 0x1000 - (uaddr & 0xFFF);
        if (chunk > n) chunk = n;
        if (to_user) memcpy((void*)(UINTN)phys, k, (size_t)chunk);
        else         memcpy(k, (const void*)(UINTN)phys, (size_t)chunk);
        k += chunk; uaddr += chunk; n -= chunk;
    }
    return 0;
}

static int copy_from_user(void *dst, UINT64 src, UINT64 n) { return user_copy(dst, src, n, 0); }
static int copy_to_user(UINT64 dst, const void *src, UINT64 n) { return user_copy((void*)src, dst, n, 1); }

// NUL sonlu dizgeyi kopyalar. Uzunluk | -1 (gecersiz adres) | -2 (cok uzun)
static int copy_str_from_user(char *dst, UINT64 src, UINT32 cap) {
    for (UINT32 i = 0; i < cap; i++) {
        if (copy_from_user(&dst[i], src + i, 1) != 0) return -1;
        if (dst[i] == 0) return (int)i;
    }
    dst[cap - 1] = 0;
    return -2;
}

static proc_t *cur(void) { return process_current(); }

static int user_path(UINT64 uptr, char *out, UINT32 cap) {
    char raw[200];
    int r = copy_str_from_user(raw, uptr, sizeof raw);
    if (r == -1) return EFAULT_RET;
    if (r < 0 || raw[0] == 0) return EINVAL_RET;
    proc_t *p = cur();
    fs_normalize(p ? p->cwd : "/", raw, out, cap);
    return 0;
}

// ------------------------------------------------------------------ dosya islemleri

static int fd_alloc(proc_t *p, file_t *f) {
    for (int i = 0; i < MAX_FD; i++) if (!p->fds[i]) { p->fds[i] = f; return i; }
    return EMFILE_RET;
}

static file_t *fd_get(proc_t *p, UINT64 fd) { return (p && fd < MAX_FD) ? p->fds[fd] : 0; }

static long long sys_open(UINT64 upath, UINT64 flags) {
    proc_t *p = cur();
    char path[160];
    int r = user_path(upath, path, sizeof path);
    if (r) return r;
    int err;
    file_t *f = file_open(path, (int)flags, &err);
    if (!f) return err;
    int fd = fd_alloc(p, f);
    if (fd < 0) { file_unref(f); return fd; }
    return fd;
}

static long long sys_close(UINT64 fd) {
    proc_t *p = cur();
    file_t *f = fd_get(p, fd);
    if (!f) return EBADF_RET;
    p->fds[fd] = 0;
    file_unref(f);
    return 0;
}

static long long sys_write(UINT64 fd, UINT64 buf, UINT64 len) {
    file_t *f = fd_get(cur(), fd);
    if (!f) return EBADF_RET;
    if (len > 65536) len = 65536;
    char tmp[512];
    UINT64 done = 0;
    while (done < len) {
        UINT64 n = len - done; if (n > sizeof tmp) n = sizeof tmp;
        if (copy_from_user(tmp, buf + done, n) != 0) return done ? (long long)done : EFAULT_RET;
        long long w = file_write(f, tmp, n);
        if (w < 0) return done ? (long long)done : w;
        done += (UINT64)w;
        if ((UINT64)w < n) break;
    }
    return (long long)done;
}

static long long sys_read(UINT64 fd, UINT64 buf, UINT64 len) {
    file_t *f = fd_get(cur(), fd);
    if (!f) return EBADF_RET;
    if (len > 65536) len = 65536;
    char tmp[512];
    UINT64 done = 0;
    while (done < len) {
        UINT64 n = len - done; if (n > sizeof tmp) n = sizeof tmp;
        long long r = file_read(f, tmp, n);
        if (r < 0) return done ? (long long)done : r;
        if (r == 0) break;
        if (copy_to_user(buf + done, tmp, (UINT64)r) != 0) return done ? (long long)done : EFAULT_RET;
        done += (UINT64)r;
        if (f->kind == FK_CON_IN || (UINT64)r < n) break;          // konsolda ilk parcadan sonra don
    }
    return (long long)done;
}

static long long sys_stat(UINT64 upath, UINT64 ubuf) {
    char path[160];
    int r = user_path(upath, path, sizeof path);
    if (r) return r;
    fs_dirent_t e;
    if (fs_stat(path, &e) != 0) return ENOENT_RET;
    ustat_t st = { e.size, (unsigned)e.is_dir, e.wdate, e.wtime };
    return copy_to_user(ubuf, &st, sizeof st) == 0 ? 0 : EFAULT_RET;
}

static long long sys_readdir(UINT64 fd, UINT64 ubuf) {
    file_t *f = fd_get(cur(), fd);
    if (!f) return EBADF_RET;
    udirent_t d;
    int r = file_readdir(f, &d);
    if (r == 1 && copy_to_user(ubuf, &d, sizeof d) != 0) return EFAULT_RET;
    return r;
}

static long long sys_path_op(UINT64 nr, UINT64 a1, UINT64 a2) {
    char p1[160], p2[160];
    int r = user_path(a1, p1, sizeof p1);
    if (r) return r;
    if (nr == SYS_MKDIR)  { int e = fs_create(p1, 1); return e ? fs_err_to_errno(e) : 0; }
    if (nr == SYS_UNLINK) { int e = fs_remove(p1);    return e ? fs_err_to_errno(e) : 0; }
    if (nr == SYS_RENAME) {
        r = user_path(a2, p2, sizeof p2);
        if (r) return r;
        int e = fs_rename(p1, p2);
        return e ? fs_err_to_errno(e) : 0;
    }
    if (nr == SYS_CHDIR) {
        fs_dirent_t d;
        if (fs_stat(p1, &d) != 0) return ENOENT_RET;
        if (!d.is_dir) return ENOTDIR_RET;
        proc_t *p = cur();
        size_t n = strlen(p1); if (n > sizeof p->cwd - 1) return EINVAL_RET;
        memcpy(p->cwd, p1, n + 1);
        return 0;
    }
    return ENOSYS_RET;
}

static long long sys_getcwd(UINT64 ubuf, UINT64 size) {
    proc_t *p = cur();
    size_t n = strlen(p->cwd) + 1;
    if (n > size) return EINVAL_RET;
    return copy_to_user(ubuf, p->cwd, n) == 0 ? (long long)(n - 1) : EFAULT_RET;
}

// ------------------------------------------------------------------ surecler

static long long sys_spawn(UINT64 upath, UINT64 uargv, UINT64 ufdmap) {
    proc_t *p = cur();
    char raw[160];
    int r = copy_str_from_user(raw, upath, sizeof raw);
    if (r == -1) return EFAULT_RET;
    if (r < 0 || !raw[0]) return EINVAL_RET;

    char *argv[MAX_ARGS + 1];
    char local[MAX_ARGS][128];
    int argc = 0;
    if (uargv) {
        for (; argc < MAX_ARGS; argc++) {
            UINT64 ptr;
            if (copy_from_user(&ptr, uargv + (UINT64)argc * 8, 8) != 0) return EFAULT_RET;
            if (!ptr) break;
            if (copy_str_from_user(local[argc], ptr, 128) < 0) return EFAULT_RET;
            argv[argc] = local[argc];
        }
    }
    argv[argc] = 0;
    int fdmap[3] = { -1, -1, -1 };
    if (ufdmap && copy_from_user(fdmap, ufdmap, sizeof fdmap) != 0) return EFAULT_RET;
    for (int k = 0; k < 3; k++) if (fdmap[k] >= MAX_FD || (fdmap[k] >= 0 && !p->fds[fdmap[k]])) return EBADF_RET;

    char path[160];
    int pid;
    if (!strchr(raw, '/')) {
        ksnprintf(path, sizeof path, "/bin/%s.elf", raw);
        pid = process_spawn_file(path, argc, argv, p->cwd, p, fdmap);
        if (pid == -1) pid = process_spawn_embedded(raw, argc, argv, p->cwd, p, fdmap);
        if (pid == -1) { fs_normalize(p->cwd, raw, path, sizeof path); pid = process_spawn_file(path, argc, argv, p->cwd, p, fdmap); }
    } else {
        fs_normalize(p->cwd, raw, path, sizeof path);
        pid = process_spawn_file(path, argc, argv, p->cwd, p, fdmap);
    }
    if (pid == -1) return ENOENT_RET;
    if (pid == -2) return EINVAL_RET;
    return pid;
}

static long long sys_wait(UINT64 pid, UINT64 ustatus) {
    if ((long long)pid <= 0) return EINVAL_RET;
    int code = 0;
    while (thread_alive((int)pid)) thread_sleep_ms(5);
    if (!thread_get_exit_code((int)pid, &code)) return ECHILD_RET;
    if (ustatus && copy_to_user(ustatus, &code, sizeof code) != 0) return EFAULT_RET;
    return (long long)pid;
}

UINT64 syscall_dispatch(UINT64 nr, UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4, UINT64 a5) {
    (void)a4; (void)a5;
    switch (nr) {
    case SYS_EXIT:    process_exit((int)a1); return 0;                 // donmez
    case SYS_WRITE:   return (UINT64)sys_write(a1, a2, a3);
    case SYS_READ:    return (UINT64)sys_read(a1, a2, a3);
    case SYS_GETPID:  return (UINT64)thread_current_id();
    case SYS_SLEEP:   thread_sleep_ms((UINT32)a1); return 0;
    case SYS_YIELD:   thread_yield(); return 0;
    case SYS_TICKS:   return timer_get_ticks();
    case SYS_GETCHAR: { int c = console_getchar(); return c >= 0 ? (UINT64)c : (UINT64)-1; }
    case SYS_SBRK:    return (UINT64)process_sbrk((long long)a1);
    case SYS_OPEN:    return (UINT64)sys_open(a1, a2);
    case SYS_CLOSE:   return (UINT64)sys_close(a1);
    case SYS_LSEEK:   { file_t *f = fd_get(cur(), a1); return f ? (UINT64)file_seek(f, (long long)a2, (int)a3) : (UINT64)EBADF_RET; }
    case SYS_STAT:    return (UINT64)sys_stat(a1, a2);
    case SYS_READDIR: return (UINT64)sys_readdir(a1, a2);
    case SYS_MKDIR: case SYS_UNLINK: case SYS_RENAME: case SYS_CHDIR:
                      return (UINT64)sys_path_op(nr, a1, a2);
    case SYS_GETCWD:  return (UINT64)sys_getcwd(a1, a2);
    case SYS_SPAWN:   return (UINT64)sys_spawn(a1, a2, a3);
    case SYS_WAIT:    return (UINT64)sys_wait(a1, a2);
    default:          return (UINT64)(long long)ENOSYS_RET;
    }
}

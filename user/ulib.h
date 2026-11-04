// DEOS -- kullanici programlari icin minik C kutuphanesi: syscall sarmalayicilari, printf, dizge islevleri.
#ifndef ULIB_H
#define ULIB_H
#include "../syscall_abi.h"

typedef unsigned long long u64;
typedef long long i64;
typedef unsigned char u8;
typedef unsigned int u32;

static inline i64 sc(i64 n, i64 a, i64 b, i64 c) {
    i64 r;
    __asm__ __volatile__ ("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory");
    return r;
}

static inline __attribute__((noreturn)) void sys_exit(int code) { sc(SYS_EXIT, code, 0, 0); for (;;) ; }
static inline i64 sys_write(int fd, const void *buf, u64 n)   { return sc(SYS_WRITE, fd, (i64)buf, (i64)n); }
static inline i64 sys_read(int fd, void *buf, u64 n)          { return sc(SYS_READ, fd, (i64)buf, (i64)n); }
static inline i64 sys_getpid(void)                            { return sc(SYS_GETPID, 0, 0, 0); }
static inline void sys_sleep(u64 ms)                          { sc(SYS_SLEEP, (i64)ms, 0, 0); }
static inline void sys_yield(void)                            { sc(SYS_YIELD, 0, 0, 0); }
static inline u64 sys_ticks(void)                             { return (u64)sc(SYS_TICKS, 0, 0, 0); }
static inline i64 sys_getchar(void)                           { return sc(SYS_GETCHAR, 0, 0, 0); }
static inline i64 sys_sbrk(i64 delta)                         { return sc(SYS_SBRK, delta, 0, 0); }

static inline i64 open(const char *path, int flags)           { return sc(SYS_OPEN, (i64)path, flags, 0); }
static inline i64 close(int fd)                               { return sc(SYS_CLOSE, fd, 0, 0); }
static inline i64 lseek(int fd, i64 off, int whence)          { return sc(SYS_LSEEK, fd, off, whence); }
static inline i64 stat(const char *path, ustat_t *st)         { return sc(SYS_STAT, (i64)path, (i64)st, 0); }
static inline i64 readdir(int fd, udirent_t *d)               { return sc(SYS_READDIR, fd, (i64)d, 0); }
static inline i64 mkdir(const char *path)                     { return sc(SYS_MKDIR, (i64)path, 0, 0); }
static inline i64 unlink(const char *path)                    { return sc(SYS_UNLINK, (i64)path, 0, 0); }
static inline i64 rename(const char *a, const char *b)        { return sc(SYS_RENAME, (i64)a, (i64)b, 0); }
static inline i64 getcwd(char *buf, u64 size)                 { return sc(SYS_GETCWD, (i64)buf, (i64)size, 0); }
static inline i64 chdir(const char *path)                     { return sc(SYS_CHDIR, (i64)path, 0, 0); }
static inline i64 spawn(const char *path, char **argv, int *fdmap) { return sc(SYS_SPAWN, (i64)path, (i64)argv, (i64)fdmap); }
static inline i64 wait(int pid, int *status)                  { return sc(SYS_WAIT, pid, (i64)status, 0); }

// dizge / bellek
u64  strlen(const char *s);
int  strcmp(const char *a, const char *b);
int  strncmp(const char *a, const char *b, u64 n);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
void *memset(void *d, int c, u64 n);
void *memcpy(void *d, const void *s, u64 n);
int  memcmp(const void *a, const void *b, u64 n);

// cikti (stdout'a tek write ile; en fazla 511 karakter)
void u_puts(const char *s);
void u_putdec(i64 v);
void u_puthex(u64 v);
int  u_printf(const char *fmt, ...);
const char *u_strerror(i64 err);

int main(int argc, char **argv);

#endif

// DEOS -- kullanici programlari icin minik kutuphane: syscall sarmalayicilari.
#ifndef ULIB_H
#define ULIB_H
#include "../syscall_abi.h"

typedef unsigned long long u64;
typedef long long i64;
typedef unsigned char u8;

static inline i64 sc(i64 n, i64 a, i64 b, i64 c) {
    i64 r;
    __asm__ __volatile__ ("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory");
    return r;
}

static inline __attribute__((noreturn)) void sys_exit(int code) { sc(SYS_EXIT, code, 0, 0); for (;;) ; }
static inline i64 sys_write(int fd, const void *buf, u64 n)   { return sc(SYS_WRITE, fd, (i64)buf, (i64)n); }
static inline i64 sys_getpid(void)                            { return sc(SYS_GETPID, 0, 0, 0); }
static inline void sys_sleep(u64 ms)                          { sc(SYS_SLEEP, (i64)ms, 0, 0); }
static inline void sys_yield(void)                            { sc(SYS_YIELD, 0, 0, 0); }
static inline u64 sys_ticks(void)                             { return (u64)sc(SYS_TICKS, 0, 0, 0); }
static inline i64 sys_getchar(void)                           { return sc(SYS_GETCHAR, 0, 0, 0); }
static inline i64 sys_sbrk(i64 delta)                         { return sc(SYS_SBRK, delta, 0, 0); }

void u_puts(const char *s);
void u_putdec(i64 v);
void u_puthex(u64 v);

int main(void);

#endif

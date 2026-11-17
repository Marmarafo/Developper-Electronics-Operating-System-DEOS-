// DEOS -- kullanici programlari icin minik C kutuphanesi: syscall sarmalayicilari, printf, dizge islevleri.
#ifndef ULIB_H
#define ULIB_H
#include "../syscall_abi.h"
#include <stdarg.h>
#include <stddef.h>

typedef unsigned long long u64;
typedef long long i64;
typedef unsigned char u8;
typedef unsigned int u32;

static inline i64 sc(i64 n, i64 a, i64 b, i64 c) {
    i64 r;
    __asm__ __volatile__ ("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory");
    return r;
}

static inline i64 sc4(i64 n, i64 a, i64 b, i64 c, i64 d) {
    i64 r;
    register i64 r10 __asm__("r10") = d;
    __asm__ __volatile__ ("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10) : "rcx", "r11", "memory");
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
static inline i64 getrandom(void *buf, u64 n)                 { return sc(SYS_RANDOM, (i64)buf, (i64)n, 0); }

// ---- ag / pipe / ortam syscall sarmalayicilari (ayrintilar syscall_abi.h'de) ----
static inline i64 socket(int type)                                  { return sc(SYS_SOCKET, type, 0, 0); }
static inline i64 bind(int fd, u32 port)                            { return sc(SYS_BIND, fd, port, 0); }
static inline i64 connect(int fd, u32 ip, u32 port)                 { return sc(SYS_CONNECT, fd, ip, port); }
static inline i64 sendto(int fd, const void *b, u64 n, const usockaddr_t *to) { return sc4(SYS_SENDTO, fd, (i64)b, (i64)n, (i64)to); }
static inline i64 recvfrom(int fd, void *b, u64 n, usockaddr_t *from)         { return sc4(SYS_RECVFROM, fd, (i64)b, (i64)n, (i64)from); }
static inline i64 sockopt(int fd, int opt, u32 val)                 { return sc(SYS_SOCKOPT, fd, opt, val); }
static inline i64 resolve(const char *name, u32 *ip)                { return sc(SYS_RESOLVE, (i64)name, (i64)ip, 0); }
static inline i64 netinfo(unetinfo_t *ni)                           { return sc(SYS_NETINFO, (i64)ni, 0, 0); }
static inline i64 pipe(int fds[2])                                  { return sc(SYS_PIPE, (i64)fds, 0, 0); }
#define IP4(a, b, c, d) ((u32)(a) | ((u32)(b) << 8) | ((u32)(c) << 16) | ((u32)(d) << 24))      // ilk oktet dusuk bayt
static inline i64 write_all(int fd, const void *b, size_t n) {          // tamamini yazar (soket/boru kismi yazabilir)
    const char *p = b; size_t d = 0;
    while (d < n) { i64 w = sys_write(fd, p + d, n - d); if (w <= 0) return -1; d += (size_t)w; }
    return (i64)d;
}
int  parse_ip(const char *s, u32 *ip);                  // "A.B.C.D" -> ip; 0 = gecerli
void format_ip(u32 ip, char out[16]);

// ---- ctype ----
int isdigit(int c); int isalpha(int c); int isalnum(int c); int isspace(int c); int isupper(int c);
int islower(int c); int isprint(int c); int isxdigit(int c); int ispunct(int c);
int toupper(int c); int tolower(int c);

// ---- stdlib ----
void *malloc(size_t n);  void free(void *p);  void *calloc(size_t a, size_t b);  void *realloc(void *p, size_t n);
size_t heap_used(void);                                  // test/tanilama: malloc'un tuttugu toplam bayt (baslik dahil)
long  strtol(const char *s, char **end, int base);       unsigned long strtoul(const char *s, char **end, int base);
int   atoi(const char *s);  long atol(const char *s);    int abs(int x);  long labs(long x);
void  qsort(void *base, size_t n, size_t sz, int (*cmp)(const void *, const void *));
void *bsearch(const void *key, const void *base, size_t n, size_t sz, int (*cmp)(const void *, const void *));
int   rand(void);  void srand(unsigned seed);
__attribute__((noreturn)) void exit(int code);           // tum FILE'lari bosaltir
__attribute__((noreturn)) void abort(void);
char *getenv(const char *name);                          // sonraki getenv cagrilarinda gecersiz olabilir (4'lu donusumlu tampon)
int   setenv(const char *name, const char *value, int overwrite);
int   unsetenv(const char *name);
int   environ_block(char *buf, size_t cap);              // "A=1\0B=2\0\0" blogunu kopyalar; uzunluk | -hata

// ---- string (ek) ----
char *strcpy(char *d, const char *s);  char *strncpy(char *d, const char *s, size_t n);
char *strcat(char *d, const char *s);  char *strncat(char *d, const char *s, size_t n);
char *strdup(const char *s);           char *strstr(const char *h, const char *n);
size_t strnlen(const char *s, size_t n);  size_t strspn(const char *s, const char *a);  size_t strcspn(const char *s, const char *r);
int   strcasecmp(const char *a, const char *b);
void *memmove(void *d, const void *s, size_t n);   void *memchr(const void *s, int c, size_t n);

// ---- stdio (bufferli FILE; kayan nokta yok) ----
typedef struct FILE FILE;
extern FILE *stdin, *stdout, *stderr;
#define EOF (-1)
#define BUFSIZ 512
#define ENV_MAX_USER 512
FILE *fopen(const char *path, const char *mode);   int fclose(FILE *f);   int fflush(FILE *f);   // fflush(NULL): hepsi
size_t fread(void *p, size_t sz, size_t n, FILE *f);   size_t fwrite(const void *p, size_t sz, size_t n, FILE *f);
int fgetc(FILE *f);  int fputc(int c, FILE *f);  int ungetc(int c, FILE *f);
char *fgets(char *s, int n, FILE *f);  int fputs(const char *s, FILE *f);
int fseek(FILE *f, long off, int whence);  long ftell(FILE *f);  void rewind(FILE *f);
int feof(FILE *f);  int ferror(FILE *f);  void clearerr(FILE *f);  int fileno(FILE *f);
int remove(const char *path);
int getchar(void);  int putchar(int c);  int puts(const char *s);
int vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap);
int snprintf(char *buf, size_t cap, const char *fmt, ...);
int sprintf(char *buf, const char *fmt, ...);
int vfprintf(FILE *f, const char *fmt, va_list ap);
int fprintf(FILE *f, const char *fmt, ...);
int printf(const char *fmt, ...);
void perror(const char *msg);

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

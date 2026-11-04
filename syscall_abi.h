// DEOS -- sistem cagrisi (syscall) numaralari. Hem kernel hem kullanici programlari kullanir.
// Cagri: rax = numara, argumanlar rdi, rsi, rdx, r10, r8; donus degeri rax.
// "syscall" talimati rcx ve r11'i bozar; digerleri korunur.
#ifndef DEOS_SYSCALL_ABI_H
#define DEOS_SYSCALL_ABI_H

#define SYS_EXIT     1   // exit(kod)                       -- donmez
#define SYS_WRITE    2   // write(fd, tampon, uzunluk)      -- fd 1/2 = konsol; yazilan bayt | -14 (EFAULT)
#define SYS_GETPID   3   // getpid()
#define SYS_SLEEP    4   // sleep(ms)
#define SYS_YIELD    5   // yield()
#define SYS_TICKS    6   // ticks()                         -- 100 Hz tick sayaci
#define SYS_GETCHAR  7   // getchar()                       -- tus | -1 (tus yok)
#define SYS_SBRK     8   // sbrk(artis)                     -- eski brk | -1; sbrk(0) = mevcut brk

#define SYS_OPEN     9   // open(yol, bayraklar, 0)         -- fd | -hata
#define SYS_CLOSE    10  // close(fd)
#define SYS_READ     11  // read(fd, tampon, n)             -- okunan bayt (0 = dosya sonu) | -hata; fd 0 klavyeden bloklar
#define SYS_LSEEK    12  // lseek(fd, ofset, nereden)       -- yeni konum | -hata
#define SYS_STAT     13  // stat(yol, ustat_t*)
#define SYS_READDIR  14  // readdir(fd, udirent_t*)         -- 1 = girdi, 0 = son, <0 hata (fd O_DIRECTORY ile acilmis olmali)
#define SYS_MKDIR    15  // mkdir(yol)
#define SYS_UNLINK   16  // unlink(yol)                     -- dosya veya BOS dizin
#define SYS_RENAME   17  // rename(eski, yeni)
#define SYS_GETCWD   18  // getcwd(tampon, boyut)           -- uzunluk | -hata
#define SYS_CHDIR    19  // chdir(yol)
#define SYS_SPAWN    20  // spawn(yol, argv, fdmap)         -- pid | -hata; fdmap: int[3] (stdin,stdout,stderr; -1 = miras)
#define SYS_WAIT     21  // wait(pid, int *durum)           -- pid | -hata

// open() bayraklari
#define O_RDONLY     0
#define O_WRONLY     1
#define O_RDWR       2
#define O_CREAT      0x40
#define O_TRUNC      0x200
#define O_APPEND     0x400
#define O_DIRECTORY  0x10000
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

// Hata kodlari (donus degeri olarak negatif)
#define ENOENT_RET  (-2)
#define EIO_RET     (-5)
#define EBADF_RET   (-9)
#define ECHILD_RET  (-10)
#define EFAULT_RET  (-14)
#define EEXIST_RET  (-17)
#define ENOTDIR_RET (-20)
#define EISDIR_RET  (-21)
#define EINVAL_RET  (-22)
#define EMFILE_RET  (-24)
#define ENOSPC_RET  (-28)
#define ENOSYS_RET  (-38)
#define ENOTEMPTY_RET (-39)

typedef struct { unsigned int size; unsigned int is_dir; unsigned int wdate; unsigned int wtime; } ustat_t;
typedef struct { char name[64]; unsigned int size; unsigned int is_dir; } udirent_t;

// Kullanici adres uzayi (her surecin KENDI PML4'u var; PML4 girdisi 1)
#define USER_BASE        0x0000008000000000ULL
#define USER_HEAP_BASE   0x0000008010000000ULL
#define USER_HEAP_MAX    0x0000008070000000ULL
#define USER_STACK_TOP   0x0000008080000000ULL
#define USER_TOP         USER_STACK_TOP

#endif

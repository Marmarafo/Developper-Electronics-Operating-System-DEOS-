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

#define ENOSYS_RET  (-38)
#define EFAULT_RET  (-14)

// Kullanici adres uzayi (her surecin KENDI PML4'u var; PML4 girdisi 1)
#define USER_BASE        0x0000008000000000ULL
#define USER_HEAP_BASE   0x0000008010000000ULL
#define USER_HEAP_MAX    0x0000008070000000ULL
#define USER_STACK_TOP   0x0000008080000000ULL
#define USER_TOP         USER_STACK_TOP

#endif

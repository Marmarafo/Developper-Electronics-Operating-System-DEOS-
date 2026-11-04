// DEOS -- sistem cagrilari (syscall). Kullanici programlari "syscall" talimatiyla
// kernel'e girer (usermode.S -> syscall_dispatch). Kullanici gostericileri ASLA
// dogrudan kullanilmaz: sayfa tablosu uzerinden dogrulanip fiziksel adres uzerinden kopyalanir.
#include "types.h"
#include "syscall.h"
#include "syscall_abi.h"
#include "cpu.h"
#include "console.h"
#include "keyboard.h"
#include "timer.h"
#include "thread.h"
#include "process.h"
#include "paging.h"
#include "string.h"

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

// ---- kullanici bellegine guvenli erisim ----

static UINT64 current_cr3(void) {
    UINT64 cr3;
    __asm__ __volatile__ ("mov %%cr3, %0" : "=r"(cr3));
    return cr3;
}

// Kullanici adresinden n bayt'i kernel tamponuna kopyalar; gecersizse -1.
static int copy_from_user(void *dst, UINT64 src, UINT64 n) {
    if (src < USER_BASE || src + n < src || src + n > USER_TOP) return -1;
    UINT64 cr3 = current_cr3();
    UINT8 *d = dst;
    while (n) {
        UINT64 phys, flags;
        UINT32 shift;
        if (!paging_query_in(cr3, src, &phys, &flags, &shift)) return -1;
        if (!(flags & PTE_U)) return -1;
        UINT64 chunk = 0x1000 - (src & 0xFFF);
        if (chunk > n) chunk = n;
        memcpy(d, (const void*)(UINTN)phys, (size_t)chunk);   // fiziksel adres birebir eslenmis
        d += chunk; src += chunk; n -= chunk;
    }
    return 0;
}

static long long sys_write(UINT64 fd, UINT64 buf, UINT64 len) {
    if (fd != 1 && fd != 2) return -9;                 // EBADF
    if (len > 4096) len = 4096;
    char tmp[129];
    UINT64 done = 0;
    while (done < len) {
        UINT64 n = len - done;
        if (n > 128) n = 128;
        if (copy_from_user(tmp, buf + done, n) != 0) return done ? (long long)done : EFAULT_RET;
        tmp[n] = 0;
        for (UINT64 i = 0; i < n; i++) console_putc(tmp[i]);
        done += n;
    }
    return (long long)done;
}

UINT64 syscall_dispatch(UINT64 nr, UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4, UINT64 a5) {
    (void)a4; (void)a5;
    switch (nr) {
    case SYS_EXIT:    process_exit((int)a1); return 0;                 // donmez
    case SYS_WRITE:   return (UINT64)sys_write(a1, a2, a3);
    case SYS_GETPID:  return (UINT64)thread_current_id();
    case SYS_SLEEP:   thread_sleep_ms((UINT32)a1); return 0;
    case SYS_YIELD:   thread_yield(); return 0;
    case SYS_TICKS:   return timer_get_ticks();
    case SYS_GETCHAR: { int c = console_getchar(); return c >= 0 ? (UINT64)c : (UINT64)-1; }
    case SYS_SBRK:    return (UINT64)process_sbrk((long long)a1);
    default:          return (UINT64)(long long)ENOSYS_RET;
    }
}

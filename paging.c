// DEOS -- sayfalama (paging). UEFI'nin biraktigi sayfa tablolarini birakip
// kendi 4 seviyeli (PML4) tablolarimizi kurar:
//   * 0 .. max(4 GB, RAM sonu) birebir (identity) eslenir, 2 MB'lik buyuk sayfalarla.
//   * RAM -> WB (normal), bos/MMIO adresler -> UC (onbellek kapali),
//     framebuffer -> WC (write-combining, PAT ile).
//   * Tum RAM calistirilamaz (NX); sadece kernel .text calistirilabilir.
//   * Kernel imaji 4 KB sayfalara bolunur: .text R-X, .rodata R--, .data/.bss RW-
//     (CR0.WP acik, yani kernel kendi salt-okunur sayfalarina da yazamaz).
//   * Sayfa 0 ESLENMEZ: NULL gostericisi kullanmak aninda page fault verir.
#include "types.h"
#include "paging.h"
#include "cpu.h"
#include "pmm.h"
#include "string.h"
#include "kprintf.h"

extern char __text_start[], __text_end[], __rodata_start[], __rodata_end[];
extern char __data_start[], __data_end[], __kernel_start[], __kernel_end[];
extern char stack_guard[];   // entry.S: boot stack'inin koruma sayfasi

#define MB2       0x200000ULL
#define GB1       0x40000000ULL
#define GB4       0x100000000ULL
#define MAX_TOP   (512ULL * GB1)
#define ADDR_MASK 0x000FFFFFFFFFF000ULL

#define MSR_EFER  0xC0000080u
#define MSR_PAT   0x277u
#define EFER_NXE  (1ULL << 11)
#define CR0_WP    (1ULL << 16)
// PAT girdisi 1 (PWT=1,PCD=0) = Write-Combining; digerleri varsayilan (WB,WT->WC,UC-,UC,...)
#define PAT_VALUE 0x0007040600070106ULL

static UINT64 *g_pml4;
static UINT64  g_table_pages;
static UINT64  g_top;
static UINT64  g_nx;                  // PTE_NX (destekleniyorsa) yoksa 0
static int     g_wp, g_wc;

static inline UINT64 rdmsr(UINT32 msr) {
    UINT32 lo, hi;
    __asm__ __volatile__ ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((UINT64)hi << 32) | lo;
}
static inline void wrmsr(UINT32 msr, UINT64 v) {
    __asm__ __volatile__ ("wrmsr" : : "c"(msr), "a"((UINT32)v), "d"((UINT32)(v >> 32)));
}
static inline void invlpg(UINT64 v) {
    __asm__ __volatile__ ("invlpg (%0)" : : "r"(v) : "memory");
}

static void halt_msg(const char *m) {
    kprintf("PAGING HATA: %s\n", m);
    while (1) { __asm__ __volatile__ ("cli; hlt"); }
}

static UINT64 *alloc_table(void) {
    UINT64 p = pmm_alloc_page();
    if (!p) halt_msg("sayfa tablosu icin bellek yok");
    memset((void*)(UINTN)p, 0, PAGE_SIZE);
    g_table_pages++;
    return (UINT64*)(UINTN)p;
}

// ---- UEFI bellek haritasi sorgulari ----
static inline UINT8 *desc_at(BootInfo *bi, UINT64 i) { return bi->memory_map + i * bi->memory_map_desc_size; }
static int is_ram_type(UINT32 t) { return (t >= 1 && t <= 7) || t == 9 || t == 10 || t == 14; }

static UINT64 ram_top(BootInfo *bi) {
    UINT64 n = bi->memory_map_size / bi->memory_map_desc_size, top = 0;
    for (UINT64 i = 0; i < n; i++) {
        UINT8 *d = desc_at(bi, i);
        if (!is_ram_type(*(UINT32*)d)) continue;
        UINT64 end = *(UINT64*)(d + 8) + *(UINT64*)(d + 24) * PAGE_SIZE;
        if (end > top) top = end;
    }
    return top;
}

static int overlaps_ram(BootInfo *bi, UINT64 a, UINT64 b) {
    UINT64 n = bi->memory_map_size / bi->memory_map_desc_size;
    for (UINT64 i = 0; i < n; i++) {
        UINT8 *d = desc_at(bi, i);
        if (!is_ram_type(*(UINT32*)d)) continue;
        UINT64 s = *(UINT64*)(d + 8), e = s + *(UINT64*)(d + 24) * PAGE_SIZE;
        if (s < b && e > a) return 1;
    }
    return 0;
}

// Bir 2 MB'lik araligin bellek turu: PWT (framebuffer WC), 0 (RAM, WB) veya PCD|PWT (UC).
static UINT64 mem_attr(BootInfo *bi, UINT64 a) {
    UINT64 b = a + MB2;
    UINT64 fb = bi->framebuffer_base, fe = fb + bi->framebuffer_size;
    if (a < fe && b > fb) return PTE_PWT;
    if (overlaps_ram(bi, a, b)) return 0;
    return PTE_PCD | PTE_PWT;
}

// 2 MB'lik bir sayfayi 512 adet 4 KB'lik girdiye boler (kernel izinleri / NULL korumasi icin).
static void split_2m(UINT64 *pd, UINT64 idx, UINT64 base, UINT64 attr) {
    UINT64 *pt = alloc_table();
    UINT64 kt0 = (UINT64)(UINTN)__text_start,   kt1 = (UINT64)(UINTN)__text_end;
    UINT64 kr0 = (UINT64)(UINTN)__rodata_start, kr1 = (UINT64)(UINTN)__rodata_end;
    UINT64 kd0 = (UINT64)(UINTN)__data_start,   kd1 = (UINT64)(UINTN)__data_end;
    for (UINT64 i = 0; i < 512; i++) {
        UINT64 a = base + i * PAGE_SIZE;
        UINT64 fl = PTE_P | PTE_W | g_nx | attr;                 // normal veri sayfasi
        if (a >= kt0 && a < kt1)      fl = PTE_P | attr;                  // .text: R-X
        else if (a >= kr0 && a < kr1) fl = PTE_P | g_nx | attr;           // .rodata: R--
        else if (a >= kd0 && a < kd1) fl = PTE_P | PTE_W | g_nx | attr;   // .data/.bss: RW-
        int unmapped = (a == 0) || (a == (UINT64)(UINTN)stack_guard);   // sayfa 0 (NULL) ve stack koruma sayfasi eslenmez
        pt[i] = unmapped ? 0 : (a | fl);
    }
    pd[idx] = (UINT64)(UINTN)pt | PTE_P | PTE_W;
}

void paging_init(BootInfo *bi) {
    // --- CPU ozellikleri ---
    UINT32 maxext, a, b, c, d;
    __asm__ __volatile__ ("cpuid" : "=a"(maxext), "=b"(b), "=c"(c), "=d"(d) : "a"(0x80000000u));
    int nx_ok = 0;
    if (maxext >= 0x80000001u) {
        __asm__ __volatile__ ("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0x80000001u));
        nx_ok = (d >> 20) & 1;
    }
    g_nx = 0;
    if (nx_ok) { wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_NXE); g_nx = PTE_NX; }   // NX bitini kullanmadan ONCE ac
    wrmsr(MSR_PAT, PAT_VALUE);                       // PAT[1] = write-combining
    __asm__ __volatile__ ("wbinvd" : : : "memory");
    g_wc = 1;

    // --- Tablolari kur ---
    g_top = ram_top(bi);
    if (g_top < GB4) g_top = GB4;          // ilk 4 GB her zaman eslenir (framebuffer, APIC, PCI...)
    if (g_top > MAX_TOP) g_top = MAX_TOP;
    g_top = (g_top + MB2 - 1) & ~(MB2 - 1);

    g_table_pages = 0;
    g_pml4 = alloc_table();
    UINT64 *pdpt = alloc_table();
    g_pml4[0] = (UINT64)(UINTN)pdpt | PTE_P | PTE_W;

    UINT64 kstart = (UINT64)(UINTN)__kernel_start, kend = (UINT64)(UINTN)__kernel_end;
    UINT64 ngb = (g_top + GB1 - 1) / GB1;
    for (UINT64 g = 0; g < ngb; g++) {
        UINT64 *pd = alloc_table();
        pdpt[g] = (UINT64)(UINTN)pd | PTE_P | PTE_W;
        for (UINT64 e = 0; e < 512; e++) {
            UINT64 base = g * GB1 + e * MB2;
            if (base >= g_top) break;
            UINT64 attr = mem_attr(bi, base);
            int need_split = (base == 0) || (base < kend && base + MB2 > kstart);
            if (need_split) split_2m(pd, e, base, attr);
            else            pd[e] = base | PTE_P | PTE_W | PTE_PS | g_nx | attr;
        }
    }

    // --- UEFI'nin tablolarini birak, bizimkini kullan ---
    UINT64 flags;
    __asm__ __volatile__ ("pushfq; pop %0; cli" : "=r"(flags) : : "memory");
    __asm__ __volatile__ ("mov %0, %%cr3" : : "r"((UINT64)(UINTN)g_pml4) : "memory");
    UINT64 cr0;
    __asm__ __volatile__ ("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= CR0_WP;                                   // kernel salt-okunur sayfalara da yazamasin
    __asm__ __volatile__ ("mov %0, %%cr0" : : "r"(cr0) : "memory");
    g_wp = 1;
    if (flags & 0x200) __asm__ __volatile__ ("sti");
}

void paging_get_info(paging_info_t *out) {
    UINT64 cr3;
    __asm__ __volatile__ ("mov %%cr3, %0" : "=r"(cr3));
    out->cr3 = cr3;
    out->mapped_top = g_top;
    out->table_pages = g_table_pages;
    out->nx = g_nx != 0;
    out->wp = g_wp;
    out->wc = g_wc;
}

int paging_query_in(UINT64 cr3, UINT64 v, UINT64 *phys, UINT64 *flags, UINT32 *shift) {
    UINT64 *pml4 = (UINT64*)(UINTN)(cr3 & ADDR_MASK);
    UINT64 e = pml4[(v >> 39) & 511];
    if (!(e & PTE_P)) return 0;
    UINT64 *t = (UINT64*)(UINTN)(e & ADDR_MASK);

    e = t[(v >> 30) & 511];
    if (!(e & PTE_P)) return 0;
    if (e & PTE_PS) { *phys = (e & 0x000FFFFFC0000000ULL) | (v & (GB1 - 1)); *flags = e; *shift = 30; return 1; }
    t = (UINT64*)(UINTN)(e & ADDR_MASK);

    e = t[(v >> 21) & 511];
    if (!(e & PTE_P)) return 0;
    if (e & PTE_PS) { *phys = (e & 0x000FFFFFFFE00000ULL) | (v & (MB2 - 1)); *flags = e; *shift = 21; return 1; }
    t = (UINT64*)(UINTN)(e & ADDR_MASK);

    e = t[(v >> 12) & 511];
    if (!(e & PTE_P)) return 0;
    *phys = (e & ADDR_MASK) | (v & 0xFFF); *flags = e; *shift = 12;
    return 1;
}

int paging_query(UINT64 v, UINT64 *phys, UINT64 *flags, UINT32 *shift) {
    return paging_query_in((UINT64)(UINTN)g_pml4, v, phys, flags, shift);
}

// Alt seviye tabloyu dondurur; yoksa olusturur; buyuk sayfa varsa 0 doner.
// user=1: tablo girdisi U bitiyle olusturulur/guncellenir (kullanici bellegi icin).
static UINT64 *next_table(UINT64 *table, UINT64 idx, int user) {
    UINT64 e = table[idx];
    if (e & PTE_P) {
        if (e & PTE_PS) return 0;
        if (user && !(e & PTE_U)) table[idx] = e | PTE_U;
        return (UINT64*)(UINTN)(e & ADDR_MASK);
    }
    UINT64 *t = alloc_table();
    table[idx] = (UINT64)(UINTN)t | PTE_P | PTE_W | (user ? PTE_U : 0);
    return t;
}

int paging_map_in(UINT64 cr3, UINT64 v, UINT64 p, UINT64 flags) {
    if ((v | p) & 0xFFF) return -1;
    int user = (flags & PTE_U) != 0;
    if (user && (v < USER_BASE_ADDR || v >= USER_TOP_ADDR)) return -1;   // kullanici sayfasi sadece kullanici bolgesine
    UINT64 *pml4 = (UINT64*)(UINTN)(cr3 & ADDR_MASK);
    UINT64 *pdpt = next_table(pml4, (v >> 39) & 511, user); if (!pdpt) return -1;
    UINT64 *pd   = next_table(pdpt, (v >> 30) & 511, user); if (!pd)   return -1;
    UINT64 *pt   = next_table(pd,   (v >> 21) & 511, user); if (!pt)   return -1;
    UINT64 idx = (v >> 12) & 511;
    if (pt[idx] & PTE_P) return -1;                 // zaten eslenmis
    if (!g_nx) flags &= ~PTE_NX;
    pt[idx] = p | flags | PTE_P;
    invlpg(v);
    return 0;
}

int paging_map_4k(UINT64 v, UINT64 p, UINT64 flags) {
    return paging_map_in((UINT64)(UINTN)g_pml4, v, p, flags);
}

UINT64 paging_kernel_cr3(void) { return (UINT64)(UINTN)g_pml4; }

UINT64 paging_new_user_space(void) {
    UINT64 p = pmm_alloc_page();
    if (!p) return 0;
    UINT64 *pml4 = (UINT64*)(UINTN)p;
    memset(pml4, 0, PAGE_SIZE);
    g_table_pages++;
    pml4[0] = g_pml4[0];                            // kernel (birebir esleme) HERKESLE ORTAK
    return p;
}

static void free_table_page(UINT64 *t) {
    pmm_free_page((UINT64)(UINTN)t);
    g_table_pages--;
}

// Surecin kullanici yarisindaki (girdi 1..255) TUM sayfalarini ve tablolarini iade eder.
void paging_destroy_user_space(UINT64 cr3) {
    UINT64 *pml4 = (UINT64*)(UINTN)(cr3 & ADDR_MASK);
    for (UINT64 i = 1; i < 256; i++) {
        if (!(pml4[i] & PTE_P)) continue;
        UINT64 *pdpt = (UINT64*)(UINTN)(pml4[i] & ADDR_MASK);
        for (UINT64 j = 0; j < 512; j++) {
            if (!(pdpt[j] & PTE_P) || (pdpt[j] & PTE_PS)) continue;
            UINT64 *pd = (UINT64*)(UINTN)(pdpt[j] & ADDR_MASK);
            for (UINT64 k = 0; k < 512; k++) {
                if (!(pd[k] & PTE_P) || (pd[k] & PTE_PS)) continue;
                UINT64 *pt = (UINT64*)(UINTN)(pd[k] & ADDR_MASK);
                for (UINT64 l = 0; l < 512; l++)
                    if (pt[l] & PTE_P) pmm_free_page(pt[l] & ADDR_MASK);
                free_table_page(pt);
            }
            free_table_page(pd);
        }
        free_table_page(pdpt);
    }
    free_table_page(pml4);
}

// 2 MB'lik buyuk sayfayi, ayni izinlerle 512 adet 4 KB'lik sayfaya boler.
static UINT64 *split_huge(UINT64 *pd, UINT64 idx) {
    UINT64 e = pd[idx];
    UINT64 base = e & 0x000FFFFFFFE00000ULL;
    UINT64 attrs = e & (PTE_W | PTE_U | PTE_PWT | PTE_PCD | PTE_NX);
    UINT64 *pt = alloc_table();
    for (UINT64 i = 0; i < 512; i++) pt[i] = (base + i * PAGE_SIZE) | PTE_P | attrs;
    pd[idx] = (UINT64)(UINTN)pt | PTE_P | PTE_W;
    return pt;
}

int paging_unmap_4k(UINT64 v) {
    UINT64 e = g_pml4[(v >> 39) & 511];
    if (!(e & PTE_P)) return -1;
    UINT64 *t = (UINT64*)(UINTN)(e & ADDR_MASK);
    e = t[(v >> 30) & 511];
    if (!(e & PTE_P) || (e & PTE_PS)) return -1;            // 1 GB sayfa: desteklenmiyor
    t = (UINT64*)(UINTN)(e & ADDR_MASK);
    UINT64 pde = t[(v >> 21) & 511];
    if (!(pde & PTE_P)) return -1;
    if (pde & PTE_PS) t = split_huge(t, (v >> 21) & 511);   // 2 MB sayfa: gerekirse bol
    else              t = (UINT64*)(UINTN)(pde & ADDR_MASK);
    if (!(t[(v >> 12) & 511] & PTE_P)) return -1;
    t[(v >> 12) & 511] = 0;
    invlpg(v);
    return 0;
}

#define MMIO_VBASE 0xFFFF900000000000ULL
static UINT64 g_mmio_next = MMIO_VBASE;

void *mmio_map(UINT64 phys, UINT64 size) {
    if (phys + size <= GB4 && phys + size <= g_top) return (void*)(UINTN)phys;     // zaten birebir, UC eslenmis
    UINT64 base = phys & ~0xFFFULL, off = phys - base;
    UINT64 pages = (off + size + PAGE_SIZE - 1) / PAGE_SIZE;
    UINT64 v = g_mmio_next;
    for (UINT64 i = 0; i < pages; i++)
        if (paging_map_4k(v + i * PAGE_SIZE, base + i * PAGE_SIZE, PTE_W | PTE_NX | PTE_PCD | PTE_PWT) != 0) return 0;
    g_mmio_next += pages * PAGE_SIZE;
    return (void*)(UINTN)(v + off);
}

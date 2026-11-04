#ifndef DEOS_PAGING_H
#define DEOS_PAGING_H
#include "types.h"
#include "bootinfo.h"

// Sayfa tablosu girdisi bitleri
#define PTE_P    (1ULL << 0)    // present
#define PTE_W    (1ULL << 1)    // yazilabilir
#define PTE_U    (1ULL << 2)    // kullanici modu erisebilir
#define PTE_PWT  (1ULL << 3)    // PAT: bizde "write-combining" (framebuffer)
#define PTE_PCD  (1ULL << 4)    // onbellek kapali (PWT ile birlikte = UC, MMIO icin)
#define PTE_PS   (1ULL << 7)    // buyuk sayfa (2 MB / 1 GB)
#define PTE_NX   (1ULL << 63)   // calistirilamaz

typedef struct {
    UINT64 cr3;
    UINT64 mapped_top;     // 0 .. mapped_top arasi birebir (identity) eslenmis
    UINT64 table_pages;    // sayfa tablolari icin kullanilan 4 KB sayfa sayisi
    int    nx, wp, wc;     // NX, CR0.WP, framebuffer icin write-combining acik mi
} paging_info_t;

// Kendi sayfa tablolarimizi kurar ve UEFI'ninkilerin yerine gecirir.
void paging_init(BootInfo *bi);
void paging_get_info(paging_info_t *out);

// Bir sanal adresi cozer: eslenmisse 1 doner; phys/flags/shift (12=4KB, 21=2MB, 30=1GB) doldurulur.
int  paging_query(UINT64 virt, UINT64 *phys, UINT64 *flags, UINT32 *shift);
// 4 KB'lik sayfa esle / eslemeyi kaldir (buyuk sayfayla kapli adreslerde -1 doner).
int  paging_map_4k(UINT64 virt, UINT64 phys, UINT64 flags);
int  paging_unmap_4k(UINT64 virt);


// ---- Surec adres uzaylari ----
// Her surecin kendi PML4'u vardir: girdi 0 (kernel, birebir esleme) TUM PML4'larla PAYLASILIR
// ve sadece kernel erisir (U=0); girdi 1+ surece ozeldir (kullanici bellegi, U=1).
#define USER_BASE_ADDR 0x0000008000000000ULL
#define USER_TOP_ADDR  0x0000008080000000ULL

UINT64 paging_kernel_cr3(void);
UINT64 paging_new_user_space(void);                 // yeni PML4 (CR3 degeri); 0 = bellek yok
void   paging_destroy_user_space(UINT64 cr3);       // kullanici sayfalarini + tablolari PMM'e iade eder
int    paging_map_in(UINT64 cr3, UINT64 virt, UINT64 phys, UINT64 flags);   // flags: PTE_U/PTE_W/PTE_NX
int    paging_query_in(UINT64 cr3, UINT64 virt, UINT64 *phys, UINT64 *flags, UINT32 *shift);


// Aygit (MMIO) bellegini onbellek kapali olarak erisilebilir yapar. Ilk 4 GB'in icindeyse birebir
// eslemeyi kullanir; degilse kernel'in yuksek yarisinda (0xFFFF9000...) yeni sayfalar esler.
void *mmio_map(UINT64 phys, UINT64 size);

#endif

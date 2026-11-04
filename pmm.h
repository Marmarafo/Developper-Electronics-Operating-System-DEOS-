#ifndef DEOS_PMM_H
#define DEOS_PMM_H
#include "types.h"
#include "bootinfo.h"

#define PAGE_SIZE 4096ULL

typedef struct {
    UINT64 range_pages;    // yonetilen adres araligi (sayfa): 0 .. en yuksek bos adres
    UINT64 usable_pages;   // baslangicta kullanilabilir sayfa (bitmap haric)
    UINT64 free_pages;     // su an bos sayfa
    UINT64 bitmap_addr;    // bitmap'in fiziksel adresi
    UINT64 bitmap_bytes;
} pmm_stats_t;

// UEFI bellek haritasindaki "Conventional" bolgeleri (>= 1 MB) bir bitmap ile yonetir.
void   pmm_init(BootInfo *bi);

// Fiziksel sayfa ayirir (4 KB hizali). Basarisizsa 0 doner.
UINT64 pmm_alloc_page(void);
// n adet BITISIK sayfa ayirir. Basarisizsa 0 doner.
UINT64 pmm_alloc_pages(UINT64 n);

void   pmm_free_page(UINT64 addr);
void   pmm_free_pages(UINT64 addr, UINT64 n);

int    pmm_page_is_free(UINT64 addr);
void   pmm_get_stats(pmm_stats_t *out);

// Sayfalamaya gectikten SONRA: UEFI BootServices bellegini havuza katar.
UINT64 pmm_reclaim_boot_services(BootInfo *bi);

#endif

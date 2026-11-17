// DEOS -- fiziksel bellek yoneticisi (PMM): bitmap tabanli sayfa ayirici.
//
// Bitmap'te 1 bit = 1 sayfa (4 KB); 1 = kullanimda, 0 = bos.
// Sadece UEFI'nin "Conventional" (tip 7) olarak isaretledigi, 1 MB'in ustundeki
// bellek bos sayilir. Bilerek GUVENLI davraniyoruz: LoaderCode/Data (kernel
// imaji, stack, bellek haritasi tamponu) ve BootServices* bolgeleri (firmware'in
// aktif sayfa tablolari orada olabilir) serbest SAYILMAZ.
// Henuz kendi sayfa tablolarimiz yok; bellek UEFI'nin birebir (identity) eslemesiyle
// erisilebilir, yani fiziksel adres == sanal adres.
#include "types.h"
#include "pmm.h"
#include "string.h"
#include "kprintf.h"
#include "cpu.h"

#define EFI_BS_CODE 3
#define EFI_BS_DATA 4
#define EFI_CONVENTIONAL 7
#define MB1  0x100000ULL
#define GB4  0x100000000ULL

static UINT8  *g_bitmap;
static UINT64  g_bitmap_bytes;
static UINT64  g_range_pages;
static UINT64  g_usable_pages;
static UINT64  g_free_pages;
static UINT64  g_hint;

static inline int  bit_test(UINT64 i)  { return (g_bitmap[i >> 3] >> (i & 7)) & 1; }
static inline void bit_set(UINT64 i)   { g_bitmap[i >> 3] |= (UINT8)(1u << (i & 7)); }
static inline void bit_clear(UINT64 i) { g_bitmap[i >> 3] &= (UINT8)~(1u << (i & 7)); }

static void halt_msg(const char *msg) {
    kprintf("PMM HATA: %s\n", msg);
    while (1) { __asm__ __volatile__ ("cli; hlt"); }
}

// UEFI bellek tanimlayicisi: +0 Type(u32), +8 PhysicalStart(u64), +24 NumberOfPages(u64).
static inline UINT8 *desc_at(BootInfo *bi, UINT64 i) {
    return bi->memory_map + i * bi->memory_map_desc_size;
}

void pmm_init(BootInfo *bi) {
    if (!bi->memory_map || !bi->memory_map_desc_size) halt_msg("bellek haritasi yok");
    UINT64 count = bi->memory_map_size / bi->memory_map_desc_size;

    // 1) En yuksek kullanilabilir adresi bul -> bitmap boyutu
    UINT64 max_end = 0;
    for (UINT64 i = 0; i < count; i++) {
        UINT8 *d = desc_at(bi, i);
        UINT32 t = *(UINT32*)d;
        if (t != EFI_CONVENTIONAL && t != EFI_BS_CODE && t != EFI_BS_DATA) continue;
        UINT64 end = *(UINT64*)(d + 8) + *(UINT64*)(d + 24) * PAGE_SIZE;
        if (end > max_end) max_end = end;
    }
    if (max_end <= MB1) halt_msg("kullanilabilir bellek yok");

    g_range_pages  = max_end / PAGE_SIZE;
    g_bitmap_bytes = (g_range_pages + 7) / 8;
    UINT64 bitmap_pages = (g_bitmap_bytes + PAGE_SIZE - 1) / PAGE_SIZE;

    // 2) Bitmap'i, yeterince buyuk ilk Conventional bolgeye (1 MB - 4 GB arasi) yerlestir
    UINT64 bitmap_addr = 0;
    for (UINT64 i = 0; i < count && !bitmap_addr; i++) {
        UINT8 *d = desc_at(bi, i);
        if (*(UINT32*)d != EFI_CONVENTIONAL) continue;
        UINT64 start = *(UINT64*)(d + 8);
        UINT64 end   = start + *(UINT64*)(d + 24) * PAGE_SIZE;
        if (start < MB1) start = MB1;
        if (end > GB4)   end = GB4;
        if (end > start && end - start >= bitmap_pages * PAGE_SIZE) bitmap_addr = start;
    }
    if (!bitmap_addr) halt_msg("bitmap icin yer bulunamadi");

    g_bitmap = (UINT8*)(UINTN)bitmap_addr;
    memset(g_bitmap, 0xFF, g_bitmap_bytes);   // once hepsi "kullanimda"

    // 3) Conventional sayfalari (>= 1 MB) bos olarak isaretle
    g_usable_pages = 0;
    for (UINT64 i = 0; i < count; i++) {
        UINT8 *d = desc_at(bi, i);
        if (*(UINT32*)d != EFI_CONVENTIONAL) continue;
        UINT64 first = *(UINT64*)(d + 8) / PAGE_SIZE;
        UINT64 n     = *(UINT64*)(d + 24);
        for (UINT64 p = first; p < first + n; p++) {
            if (p * PAGE_SIZE < MB1) continue;
            bit_clear(p);
            g_usable_pages++;
        }
    }

    // 4) Bitmap'in kendi sayfalarini kullanimda isaretle
    UINT64 bfirst = bitmap_addr / PAGE_SIZE;
    for (UINT64 p = bfirst; p < bfirst + bitmap_pages; p++) {
        if (!bit_test(p)) { bit_set(p); g_usable_pages--; }
    }

    g_free_pages = g_usable_pages;
    g_hint = MB1 / PAGE_SIZE;
}

// [from, to) araliginda n bitisik bos sayfa ara; bulursan baslangic sayfasini dondur.
static UINT64 find_run(UINT64 from, UINT64 to, UINT64 n) {
    UINT64 run = 0, start = 0;
    for (UINT64 i = from; i < to; i++) {
        if (!bit_test(i)) {
            if (run == 0) start = i;
            if (++run == n) return start + 1;   // +1: 0 "bulunamadi" demek olsun
        } else {
            run = 0;
        }
    }
    return 0;
}

static UINT64 alloc_pages_nolock(UINT64 n) {
    if (n == 0 || n > g_free_pages) return 0;
    UINT64 r = find_run(g_hint, g_range_pages, n);
    if (!r) r = find_run(MB1 / PAGE_SIZE, g_range_pages, n);   // basa sar
    if (!r) return 0;
    UINT64 page = r - 1;
    for (UINT64 p = page; p < page + n; p++) bit_set(p);
    g_free_pages -= n;
    g_hint = page + n;
    return page * PAGE_SIZE;
}



static void free_pages_nolock(UINT64 addr, UINT64 n) {
    UINT64 first = addr / PAGE_SIZE;
    if ((addr & (PAGE_SIZE - 1)) || first + n > g_range_pages || first < MB1 / PAGE_SIZE) {
        kprintf("pmm: gecersiz free(0x%llx, %llu)\n", addr, n);
        return;
    }
    for (UINT64 p = first; p < first + n; p++) {
        if (!bit_test(p)) { kprintf("pmm: cift free 0x%llx\n", p * PAGE_SIZE); continue; }
        bit_clear(p);
        g_free_pages++;
    }
    if (first < g_hint) g_hint = first;
}

int pmm_page_is_free(UINT64 addr) {
    UINT64 p = addr / PAGE_SIZE;
    return p < g_range_pages && !bit_test(p);
}

void pmm_get_stats(pmm_stats_t *out) {
    out->range_pages  = g_range_pages;
    out->usable_pages = g_usable_pages;
    out->free_pages   = g_free_pages;
    out->bitmap_addr  = (UINT64)(UINTN)g_bitmap;
    out->bitmap_bytes = g_bitmap_bytes;
}

// UEFI "BootServicesCode/Data" bolgelerini bos havuza katar. SADECE kendi sayfa
// tablolarimiza gectikten sonra cagrilmalidir: UEFI'nin eski sayfa tablolari
// (ve GDT/IDT'si) bu bolgelerde olabilir. Geri kazanilan sayfa sayisini dondurur.
UINT64 pmm_reclaim_boot_services(BootInfo *bi) {
    UINT64 count = bi->memory_map_size / bi->memory_map_desc_size;
    UINT64 reclaimed = 0;
    for (UINT64 i = 0; i < count; i++) {
        UINT8 *d = desc_at(bi, i);
        UINT32 t = *(UINT32*)d;
        if (t != EFI_BS_CODE && t != EFI_BS_DATA) continue;
        UINT64 first = *(UINT64*)(d + 8) / PAGE_SIZE;
        UINT64 n     = *(UINT64*)(d + 24);
        for (UINT64 p = first; p < first + n && p < g_range_pages; p++) {
            if (p * PAGE_SIZE < MB1 || !bit_test(p)) continue;
            bit_clear(p);
            reclaimed++;
        }
    }
    g_usable_pages += reclaimed;
    g_free_pages   += reclaimed;
    if (g_hint > MB1 / PAGE_SIZE) g_hint = MB1 / PAGE_SIZE;
    return reclaimed;
}

// Thread'ler arasi guvenlik: PMM islemleri kesmeler kapaliyken yapilir.
UINT64 pmm_alloc_pages(UINT64 n) { UINT64 f = irq_save(); UINT64 r = alloc_pages_nolock(n); irq_restore(f); return r; }
UINT64 pmm_alloc_page(void)      { return pmm_alloc_pages(1); }
void   pmm_free_pages(UINT64 addr, UINT64 n) { UINT64 f = irq_save(); free_pages_nolock(addr, n); irq_restore(f); }
void   pmm_free_page(UINT64 addr) { pmm_free_pages(addr, 1); }

#ifndef DEOS_HEAP_H
#define DEOS_HEAP_H
#include "types.h"
#include "string.h"

typedef struct {
    UINT64 total_bytes;    // heap'in toplam boyutu
    UINT64 used_bytes;     // kullanimdaki veri (baslik haric)
    UINT64 free_bytes;     // bos bloklardaki veri alani
    UINT64 blocks;         // toplam blok sayisi
    UINT64 free_blocks;
} heap_stats_t;

// PMM'den bitisik sayfalar alip heap kurar. Basarisizsa 0 doner.
int   heap_init(UINT64 bytes);
void *kmalloc(size_t size);          // 16 bayt hizali; yer yoksa NULL
void *kcalloc(size_t n, size_t sz);  // sifirlanmis
void *krealloc(void *p, size_t size);
void  kfree(void *p);
void  heap_get_stats(heap_stats_t *out);

#endif

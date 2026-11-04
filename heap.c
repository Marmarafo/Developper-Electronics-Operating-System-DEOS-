// DEOS -- kernel heap: kmalloc/kfree. Birlestirmeli (coalescing) ilk-uyan
// (first-fit) liste ayirici. Her blogun onunde 32 baytlik baslik bulunur.
#include "types.h"
#include "heap.h"
#include "pmm.h"
#include "kprintf.h"
#include "cpu.h"

#define MAGIC_USED 0xA110C8EDu
#define MAGIC_FREE 0xF4EEB10Cu
#define ALIGN      16ULL

typedef struct block {
    UINT64 size;            // veri alani boyutu (baslik haric)
    UINT32 magic;
    UINT32 pad;
    struct block *next;
    struct block *prev;
} block_t;                  // 32 bayt

static block_t *g_head;
static UINT64   g_total;

int heap_init(UINT64 bytes) {
    UINT64 pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    UINT64 base = 0;
    while (pages >= 16 && !(base = pmm_alloc_pages(pages))) pages /= 2;   // yer yoksa kucult
    if (!base) return 0;

    g_head = (block_t*)(UINTN)base;
    g_total = pages * PAGE_SIZE;
    g_head->size  = g_total - sizeof(block_t);
    g_head->magic = MAGIC_FREE;
    g_head->pad   = 0;
    g_head->next  = 0;
    g_head->prev  = 0;
    return 1;
}

static void *kmalloc_nolock(size_t size) {
    if (!size || !g_head) return 0;
    UINT64 need = ((UINT64)size + ALIGN - 1) & ~(ALIGN - 1);
    if (need > g_total) return 0;

    for (block_t *b = g_head; b; b = b->next) {
        if (b->magic != MAGIC_FREE || b->size < need) continue;
        // Artan kisim yeni bir bos blok olabilecek kadar buyukse bol
        if (b->size >= need + sizeof(block_t) + ALIGN) {
            block_t *n = (block_t*)((UINT8*)(b + 1) + need);
            n->size  = b->size - need - sizeof(block_t);
            n->magic = MAGIC_FREE;
            n->pad   = 0;
            n->next  = b->next;
            n->prev  = b;
            if (b->next) b->next->prev = n;
            b->next = n;
            b->size = need;
        }
        b->magic = MAGIC_USED;
        return b + 1;
    }
    return 0;
}

static void kfree_nolock(void *p) {
    if (!p) return;
    block_t *b = (block_t*)p - 1;
    if (b->magic == MAGIC_FREE) { kprintf("heap: cift kfree(%p)\n", p); return; }
    if (b->magic != MAGIC_USED) { kprintf("heap: bozuk blok/gecersiz kfree(%p)\n", p); return; }
    b->magic = MAGIC_FREE;

    // Sonraki bos blokla birlestir
    if (b->next && b->next->magic == MAGIC_FREE) {
        b->size += sizeof(block_t) + b->next->size;
        b->next = b->next->next;
        if (b->next) b->next->prev = b;
    }
    // Onceki bos blokla birlestir
    if (b->prev && b->prev->magic == MAGIC_FREE) {
        b->prev->size += sizeof(block_t) + b->size;
        b->prev->next = b->next;
        if (b->next) b->next->prev = b->prev;
    }
}

void *kcalloc(size_t n, size_t sz) {
    if (sz && n > (size_t)-1 / sz) return 0;   // tasma
    size_t total = n * sz;
    void *p = kmalloc(total);
    if (p) memset(p, 0, total);
    return p;
}

void *krealloc(void *p, size_t size) {
    if (!p) return kmalloc(size);
    if (!size) { kfree(p); return 0; }
    block_t *b = (block_t*)p - 1;
    if (b->magic != MAGIC_USED) return 0;
    if (b->size >= size) return p;
    void *n = kmalloc(size);
    if (!n) return 0;
    memcpy(n, p, (size_t)b->size);
    kfree(p);
    return n;
}

static void stats_nolock(heap_stats_t *out) {
    out->total_bytes = g_total;
    out->used_bytes = out->free_bytes = out->blocks = out->free_blocks = 0;
    for (block_t *b = g_head; b; b = b->next) {
        out->blocks++;
        if (b->magic == MAGIC_FREE) { out->free_bytes += b->size; out->free_blocks++; }
        else                         out->used_bytes += b->size;
    }
}

// Thread'ler arasi guvenlik: heap islemleri kesmeler kapaliyken yapilir.
void *kmalloc(size_t size) { UINT64 f = irq_save(); void *r = kmalloc_nolock(size); irq_restore(f); return r; }
void  kfree(void *p)       { UINT64 f = irq_save(); kfree_nolock(p); irq_restore(f); }
void  heap_get_stats(heap_stats_t *out) { UINT64 f = irq_save(); stats_nolock(out); irq_restore(f); }

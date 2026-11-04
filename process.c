// DEOS -- surec yonetimi: ELF'i yeni bir adres uzayina yukler, kullanici stack'i kurar,
// ring 3'te calisan bir thread baslatir. Sonlaninca (exit / hata / kill) adres uzayi
// zamanlayici tarafindan (thread.c reap) tamamen PMM'e iade edilir.
#include "types.h"
#include "process.h"
#include "elf.h"
#include "paging.h"
#include "pmm.h"
#include "heap.h"
#include "thread.h"
#include "kprintf.h"
#include "string.h"
#include "syscall_abi.h"
#include "fs.h"

#define USER_STACK_PAGES 16     // 64 KB

typedef struct { UINT64 entry, rsp; } start_info_t;

typedef struct { UINT64 heap_base, brk; } proc_t;

extern void enter_user(UINT64 rip, UINT64 rsp);                 // usermode.S
extern const char *kernel_exception_name(UINT64 vector);        // kernel.c

const embedded_prog_t *prog_find(const char *name) {
    for (const embedded_prog_t *p = g_progs; p->name; p++)
        if (!strcmp(p->name, name)) return p;
    return 0;
}

// Yeni thread'in ilk calismasi: zaten surecin kendi CR3'u aktif; ring 3'e atla.
static void user_thunk(void *arg) {
    start_info_t info = *(start_info_t*)arg;
    kfree(arg);
    enter_user(info.entry, info.rsp);
}

int process_start(const char *name, const UINT8 *elf, UINT64 size) {
    UINT64 cr3 = paging_new_user_space();
    if (!cr3) return -1;

    UINT64 entry;
    if (elf_load(elf, size, cr3, &entry) != 0) { paging_destroy_user_space(cr3); return -1; }

    for (UINT64 i = 1; i <= USER_STACK_PAGES; i++) {
        UINT64 page = pmm_alloc_page();
        if (!page) { paging_destroy_user_space(cr3); return -1; }
        memset((void*)(UINTN)page, 0, PAGE_SIZE);
        if (paging_map_in(cr3, USER_STACK_TOP - i * PAGE_SIZE, page, PTE_U | PTE_W | PTE_NX) != 0) {
            pmm_free_page(page);
            paging_destroy_user_space(cr3);
            return -1;
        }
    }

    proc_t *proc = kcalloc(1, sizeof(proc_t));
    start_info_t *si = kmalloc(sizeof(start_info_t));
    if (!proc || !si) { kfree(proc); kfree(si); paging_destroy_user_space(cr3); return -1; }
    proc->heap_base = proc->brk = USER_HEAP_BASE;
    si->entry = entry;
    si->rsp = USER_STACK_TOP - 8;       // "call" ile girilmis gibi: rsp = 8 (mod 16)

    int id = thread_create_user(name, user_thunk, si, cr3, proc);
    if (id < 0) { kfree(proc); kfree(si); paging_destroy_user_space(cr3); return -1; }
    return id;
}

int process_start_by_name(const char *name) {
    const embedded_prog_t *p = prog_find(name);
    if (!p) return -1;
    int pid = process_start(name, p->start, (UINT64)(p->end - p->start));
    return pid < 0 ? -2 : pid;
}

int process_start_file(const char *path) {
    UINT64 size;
    UINT8 *img = fs_read_all(path, &size);
    if (!img) return -1;
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    char name[16];
    size_t n = 0;
    while (base[n] && base[n] != '.' && n < 15) { name[n] = base[n]; n++; }
    name[n] = 0;
    int pid = process_start(name[0] ? name : "program", img, size);
    kfree(img);
    return pid < 0 ? -2 : pid;
}

long long process_sbrk(long long delta) {
    proc_t *p = thread_current_proc();
    if (!p) return -1;
    UINT64 old = p->brk;
    if (delta == 0) return (long long)old;
    if (delta < 0) return -1;                                   // kuculme henuz yok
    UINT64 nw = old + (UINT64)delta;
    if (nw < old || nw > USER_HEAP_MAX) return -1;

    UINT64 cr3;
    __asm__ __volatile__ ("mov %%cr3, %0" : "=r"(cr3));
    for (UINT64 va = (old + 0xFFF) & ~0xFFFULL; va < nw; va += PAGE_SIZE) {
        UINT64 page = pmm_alloc_page();
        if (!page) return -1;
        memset((void*)(UINTN)page, 0, PAGE_SIZE);
        if (paging_map_in(cr3, va, page, PTE_U | PTE_W | PTE_NX) != 0) { pmm_free_page(page); return -1; }
    }
    p->brk = nw;
    return (long long)old;
}

void process_exit(int code) {
    thread_set_exit_code(code);
    thread_exit();
    for (;;) __asm__ __volatile__ ("hlt");
}

void process_user_fault(isr_frame_t *f) {
    UINT64 cr2;
    __asm__ __volatile__ ("mov %%cr2, %0" : "=r"(cr2));
    kprintf("\n[pid %d] KULLANICI MODU HATASI: %s\n", thread_current_id(), kernel_exception_name(f->int_no));
    kprintf("  RIP=0x%llx  hata kodu=0x%llx\n", f->rip, f->err_code);
    if (f->int_no == 14) {
        kprintf("  erisilmek istenen adres: 0x%llx  (%s, %s)\n", cr2,
                (f->err_code & 1) ? "koruma ihlali" : "sayfa eslenmemis",
                (f->err_code & 16) ? "komut getirme" : ((f->err_code & 2) ? "yazma" : "okuma"));
    }
    kprintf("  -> surec sonlandirildi; kernel calismaya devam ediyor.\n");
    thread_set_exit_code(-1);
    thread_exit();
    for (;;) __asm__ __volatile__ ("hlt");
}

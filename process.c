// DEOS -- surec yonetimi: ELF'i yeni bir adres uzayina yukler, argc/argv ile kullanici stack'i kurar,
// fd tablosunu ve calisma dizinini hazirlar, ring 3'te calisan bir thread baslatir. Sonlaninca
// (exit / hata / kill) adres uzayi ve dosyalar zamanlayici tarafindan (thread.c reap) iade edilir.
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

typedef struct { UINT64 entry, rsp, argc, argv; } start_info_t;

extern void enter_user(UINT64 rip, UINT64 rsp, UINT64 arg1, UINT64 arg2);     // usermode.S
extern const char *kernel_exception_name(UINT64 vector);                       // kernel.c

const embedded_prog_t *prog_find(const char *name) {
    for (const embedded_prog_t *p = g_progs; p->name; p++)
        if (!strcmp(p->name, name)) return p;
    return 0;
}

proc_t *process_current(void) { return thread_current_proc(); }

// Yeni thread'in ilk calismasi: zaten surecin kendi CR3'u aktif; ring 3'e atla.
static void user_thunk(void *arg) {
    start_info_t info = *(start_info_t*)arg;
    kfree(arg);
    enter_user(info.entry, info.rsp, info.argc, info.argv);
}

// argc/argv'yi stack'in en ust sayfasina yazar. 0 = sigmadi.
static int build_args(UINT64 top_page_phys, int argc, char **argv, UINT64 *sp, UINT64 *argv_va) {
    UINT8 *pg = (UINT8*)(UINTN)top_page_phys;
    UINT64 page_va = USER_STACK_TOP - PAGE_SIZE;
    UINT64 off = PAGE_SIZE, str_va[MAX_ARGS];
    if (argc > MAX_ARGS) argc = MAX_ARGS;
    for (int i = 0; i < argc; i++) {
        UINT64 len = strlen(argv[i]) + 1;
        if (off < len + 512) return 0;
        off -= len;
        memcpy(pg + off, argv[i], (size_t)len);
        str_va[i] = page_va + off;
    }
    off &= ~15ULL;
    off -= (UINT64)(argc + 1) * 8;
    off &= ~15ULL;
    UINT64 *arr = (UINT64*)(pg + off);
    for (int i = 0; i < argc; i++) arr[i] = str_va[i];
    arr[argc] = 0;
    *argv_va = page_va + off;
    *sp = ((page_va + off) - 64) & ~15ULL;
    *sp -= 8;                                        // "call" ile girilmis gibi: rsp = 8 (mod 16)
    return 1;
}

static int spawn_image(const char *name, const UINT8 *elf, UINT64 size, int argc, char **argv,
                       const char *cwd, proc_t *parent, const int *fdmap) {
    UINT64 cr3 = paging_new_user_space();
    if (!cr3) return -2;

    UINT64 entry;
    if (elf_load(elf, size, cr3, &entry) != 0) { paging_destroy_user_space(cr3); return -2; }

    UINT64 top_page = 0;
    for (UINT64 i = 1; i <= USER_STACK_PAGES; i++) {
        UINT64 page = pmm_alloc_page();
        if (!page) { paging_destroy_user_space(cr3); return -2; }
        memset((void*)(UINTN)page, 0, PAGE_SIZE);
        if (paging_map_in(cr3, USER_STACK_TOP - i * PAGE_SIZE, page, PTE_U | PTE_W | PTE_NX) != 0) {
            pmm_free_page(page);
            paging_destroy_user_space(cr3);
            return -2;
        }
        if (i == 1) top_page = page;
    }

    UINT64 sp, argv_va;
    if (!build_args(top_page, argc, argv, &sp, &argv_va)) { paging_destroy_user_space(cr3); return -2; }

    proc_t *proc = kcalloc(1, sizeof(proc_t));
    start_info_t *si = kmalloc(sizeof(start_info_t));
    if (!proc || !si) { kfree(proc); kfree(si); paging_destroy_user_space(cr3); return -2; }
    proc->heap_base = proc->brk = USER_HEAP_BASE;
    size_t cl = strlen(cwd); if (cl > sizeof proc->cwd - 1) cl = sizeof proc->cwd - 1;
    memcpy(proc->cwd, cwd, cl); proc->cwd[cl] = 0;
    for (int k = 0; k < 3; k++) {                    // stdin/stdout/stderr: parent'in fd'leri (fdmap) veya konsol
        file_t *f = 0;
        if (parent) {
            int pfd = fdmap ? fdmap[k] : -1;
            if (pfd < 0) pfd = k;
            if (pfd < MAX_FD) f = parent->fds[pfd];
        }
        if (f) file_ref(f); else f = file_console(k);
        proc->fds[k] = f;
    }
    si->entry = entry; si->rsp = sp; si->argc = (UINT64)argc; si->argv = argv_va;

    int id = thread_create_user(name, user_thunk, si, cr3, proc);
    if (id < 0) { process_free(proc); kfree(si); paging_destroy_user_space(cr3); return -2; }
    return id;
}

static void thread_name(const char *path, char out[16]) {
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    size_t n = 0;
    while (base[n] && base[n] != '.' && n < 15) { out[n] = base[n]; n++; }
    out[n] = 0;
    if (!n) memcpy(out, "program", 8);
}

int process_spawn_file(const char *path, int argc, char **argv, const char *cwd, proc_t *parent, const int *fdmap) {
    UINT64 size;
    UINT8 *img = fs_read_all(path, &size);
    if (!img) return -1;
    char name[16];
    thread_name(path, name);
    char *def[2] = { (char*)path, 0 };
    if (argc <= 0 || !argv) { argc = 1; argv = def; }
    int pid = spawn_image(name, img, size, argc, argv, cwd, parent, fdmap);
    kfree(img);
    return pid;
}

int process_spawn_embedded(const char *name, int argc, char **argv, const char *cwd, proc_t *parent, const int *fdmap) {
    const embedded_prog_t *p = prog_find(name);
    if (!p) return -1;
    char *def[2] = { (char*)name, 0 };
    if (argc <= 0 || !argv) { argc = 1; argv = def; }
    return spawn_image(name, p->start, (UINT64)(p->end - p->start), argc, argv, cwd, parent, fdmap);
}

int process_start(const char *name, const UINT8 *elf, UINT64 size) {
    char *argv[2] = { (char*)name, 0 };
    return spawn_image(name, elf, size, 1, argv, "/", 0, 0);
}

int process_start_by_name(const char *name) { return process_spawn_embedded(name, 0, 0, "/", 0, 0); }
int process_start_file(const char *path) { return process_spawn_file(path, 0, 0, "/", 0, 0); }

void process_free(void *p) {
    proc_t *proc = p;
    if (!proc) return;
    for (int i = 0; i < MAX_FD; i++) if (proc->fds[i]) file_unref(proc->fds[i]);
    kfree(proc);
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

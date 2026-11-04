// DEOS -- thread zamanlayici (scheduler) ve surec altyapisi.
//
// * Her thread'in kendi kernel stack'i vardir: [koruma sayfasi][32 KB stack].
//   Koruma sayfasi eslenmez; tasarsa page fault -> #DF -> IST stack'inde panik.
// * Timer kesmesi (100 Hz) her tick'te schedule()'u cagirir: round-robin, 10 ms dilim.
// * Tum zamanlayici islemleri kesmeler KAPALIYKEN yapilir (tek islemci = kilit gerekmez).
// * Hicbir thread hazir degilse "idle" thread'i calisir (sti; hlt dongusu).
// * Thread degisiminde: CR3 (surecin adres uzayi), TSS.rsp0 (ring3->0 gecis stack'i) ve
//   syscall kernel stack'i o thread'inkine ayarlanir.
#include "types.h"
#include "thread.h"
#include "pmm.h"
#include "heap.h"
#include "paging.h"
#include "timer.h"
#include "gdt.h"
#include "syscall.h"
#include "kprintf.h"
#include "string.h"
#include "cpu.h"

#define STACK_PAGES 8                 // 32 KB (+ 1 koruma sayfasi)
#define TIMER_HZ    100
#define EXIT_RING   16

typedef enum { T_READY, T_RUNNING, T_SLEEPING, T_DEAD } tstate_t;

struct thread {
    UINT64 rsp;                       // kaydedilmis stack gostericisi (switch_context)
    int    id;
    tstate_t state;
    int    is_idle;
    char   name[16];
    UINT64 wake_tick;
    UINT64 cpu_ticks;
    UINT64 stack_base;                // koruma sayfasi dahil taban; 0 = boot stack
    UINT64 kstack_top;                // bu thread'in kernel stack'inin ustu
    UINT64 cr3;                       // adres uzayi (kernel thread'leri: kernel PML4)
    void  *proc;                      // kullanici sureci verisi (yoksa 0)
    int    exit_code;
    struct thread *next;              // dairesel liste
};
typedef struct thread thread_t;

extern void switch_context(UINT64 *old_rsp, UINT64 new_rsp);
extern void thread_trampoline(void);
extern char stack_top[];              // entry.S: boot stack'inin ustu

static thread_t  g_main;              // thread 0: kernel_main (boot stack'inde calisir)
static thread_t *g_current;
static thread_t *g_idle;
static int       g_next_id = 1;
static int       g_started;
static UINT64    g_kernel_cr3, g_cur_cr3;

static struct { int id; int code; } g_exits[EXIT_RING];
static int g_exit_n;

static void record_exit(int id, int code) {
    g_exits[g_exit_n % EXIT_RING].id = id;
    g_exits[g_exit_n % EXIT_RING].code = code;
    g_exit_n++;
}

static thread_t *find_thread(int id) {
    thread_t *t = &g_main;
    do { if (t->id == id && t->state != T_DEAD) return t; t = t->next; } while (t != &g_main);
    return 0;
}

// Olu thread'lerin stack'ini, adres uzayini ve yapisini serbest birakir. Kesmeler kapaliyken cagrilir.
static void reap(void) {
    thread_t *prev = &g_main, *t = g_main.next;
    while (t != &g_main) {
        thread_t *nx = t->next;
        if (t->state == T_DEAD && t != g_current) {
            prev->next = nx;
            if (t->cr3 != g_kernel_cr3) paging_destroy_user_space(t->cr3);   // surecin tum kullanici bellegi
            if (t->proc) kfree(t->proc);
            paging_map_in(g_kernel_cr3, t->stack_base, t->stack_base, PTE_W | PTE_NX);   // koruma sayfasini geri esle
            pmm_free_pages(t->stack_base, 1 + STACK_PAGES);
            kfree(t);
        } else {
            prev = t;
        }
        t = nx;
    }
}

// Siradaki thread'i secip ona gecer. KESMELER KAPALIYKEN cagrilmalidir.
static void schedule(void) {
    thread_t *prev = g_current;
    UINT64 now = timer_get_ticks();

    thread_t *t = &g_main;
    do {
        if (t->state == T_SLEEPING && now >= t->wake_tick) t->state = T_READY;
        t = t->next;
    } while (t != &g_main);

    if (prev->state == T_RUNNING) prev->state = T_READY;

    thread_t *n = g_idle;                       // hazir thread yoksa idle
    t = prev->next;
    for (;;) {
        if (t != g_idle && t->state == T_READY) { n = t; break; }
        if (t == prev) break;
        t = t->next;
    }
    n->state = T_RUNNING;
    if (n == prev) return;

    g_current = n;
    if (n->cr3 != g_cur_cr3) {                  // surec degisimi: adres uzayini degistir (kernel her yerde ayni)
        g_cur_cr3 = n->cr3;
        __asm__ __volatile__ ("mov %0, %%cr3" : : "r"(n->cr3) : "memory");
    }
    tss_set_rsp0(n->kstack_top);                // ring 3'ten gelen kesme bu thread'in stack'ine insin
    syscall_set_kstack(n->kstack_top);          // syscall girisi de
    switch_context(&prev->rsp, n->rsp);
    reap();                                     // (bu thread tekrar calisinca)
}

static void idle_fn(void *arg) {
    (void)arg;
    for (;;) __asm__ __volatile__ ("sti; hlt");
}

void thread_init(void) {
    memset(&g_main, 0, sizeof g_main);
    g_main.id = 0;
    g_main.state = T_RUNNING;
    g_main.next = &g_main;
    memcpy(g_main.name, "shell", 6);
    g_kernel_cr3 = g_cur_cr3 = paging_kernel_cr3();
    g_main.cr3 = g_kernel_cr3;
    g_main.kstack_top = (UINT64)(UINTN)stack_top;
    g_current = &g_main;
    tss_set_rsp0(g_main.kstack_top);
    syscall_set_kstack(g_main.kstack_top);

    int id = thread_create("idle", idle_fn, 0);
    g_idle = find_thread(id);
    g_idle->is_idle = 1;
    g_started = 1;
}

int thread_create_user(const char *name, void (*fn)(void *), void *arg, UINT64 cr3, void *proc) {
    UINT64 f = irq_save();
    thread_t *t = kmalloc(sizeof(thread_t));
    UINT64 base = pmm_alloc_pages(1 + STACK_PAGES);
    if (!t || !base) {
        if (t) kfree(t);
        if (base) pmm_free_pages(base, 1 + STACK_PAGES);
        irq_restore(f);
        return -1;
    }
    paging_unmap_4k(base);                       // en alt sayfa = koruma sayfasi

    memset(t, 0, sizeof *t);
    size_t n = strlen(name); if (n > 15) n = 15;
    memcpy(t->name, name, n);
    t->id = g_next_id++;
    t->state = T_READY;
    t->stack_base = base;
    t->cr3 = cr3 ? cr3 : g_kernel_cr3;
    t->proc = proc;

    // Ilk calisma icin stack karesi: switch_context'in pop'layip "ret" yapacagi sekilde
    UINT64 top = base + (1 + STACK_PAGES) * PAGE_SIZE;     // 16 bayt hizali
    t->kstack_top = top;
    UINT64 *sp = (UINT64*)(UINTN)(top - 16 - 56);
    sp[0] = 0;                                   // r15
    sp[1] = 0;                                   // r14
    sp[2] = (UINT64)(UINTN)arg;                  // r13 -> trampoline'de rdi
    sp[3] = (UINT64)(UINTN)fn;                   // r12 -> trampoline'de cagrilir
    sp[4] = 0;                                   // rbx
    sp[5] = 0;                                   // rbp
    sp[6] = (UINT64)(UINTN)thread_trampoline;    // ret adresi
    t->rsp = (UINT64)(UINTN)sp;

    t->next = g_main.next;                       // listeye ekle
    g_main.next = t;
    int id = t->id;
    irq_restore(f);
    return id;
}

int thread_create(const char *name, void (*fn)(void *), void *arg) {
    return thread_create_user(name, fn, arg, 0, 0);
}

void thread_yield(void) {
    UINT64 f = irq_save();
    schedule();
    irq_restore(f);
}

void thread_sleep_ms(UINT32 ms) {
    UINT64 ticks = ((UINT64)ms * TIMER_HZ + 999) / 1000;
    if (ticks == 0) ticks = 1;
    UINT64 f = irq_save();
    g_current->wake_tick = timer_get_ticks() + ticks;
    g_current->state = T_SLEEPING;
    schedule();
    irq_restore(f);
}

void thread_exit(void) {
    irq_save();                                  // geri donmeyecegiz: kesmeleri acmiyoruz
    if (g_current == &g_main || g_current->is_idle) { kprintf("thread: shell/idle bitirilemez\n"); return; }
    record_exit(g_current->id, g_current->exit_code);
    g_current->state = T_DEAD;
    schedule();
    for (;;) __asm__ __volatile__ ("hlt");
}

int thread_kill(int id) {
    UINT64 f = irq_save();
    thread_t *t = find_thread(id);
    if (!t || t == &g_main || t->is_idle || t == g_current) { irq_restore(f); return -1; }
    record_exit(t->id, -9);
    t->state = T_DEAD;                           // kaynaklari bir sonraki schedule()'da reap() serbest birakir
    irq_restore(f);
    return 0;
}

void thread_preempt(void) {
    if (!g_started) return;
    g_current->cpu_ticks++;
    schedule();
}

int thread_count(void) {
    UINT64 f = irq_save();
    int n = 0;
    thread_t *t = &g_main;
    do { if (t->state != T_DEAD && !t->is_idle) n++; t = t->next; } while (t != &g_main);
    irq_restore(f);
    return n;
}

int thread_alive(int id) {
    UINT64 f = irq_save();
    int r = find_thread(id) != 0;
    irq_restore(f);
    return r;
}

int   thread_current_id(void)   { return g_current->id; }
void *thread_current_proc(void) { return g_current->proc; }
void  thread_set_exit_code(int code) { g_current->exit_code = code; }

int thread_get_exit_code(int id, int *code) {
    UINT64 f = irq_save();
    int found = 0;
    for (int i = 0; i < EXIT_RING && i < g_exit_n; i++) {
        int idx = (g_exit_n - 1 - i) % EXIT_RING;     // en yeniden eskiye
        if (g_exits[idx].id == id) { *code = g_exits[idx].code; found = 1; break; }
    }
    irq_restore(f);
    return found;
}

void thread_dump(void) {
    static const char *names[] = { "READY", "RUNNING", "SLEEPING", "DEAD" };
    UINT64 f = irq_save();
    kprintf("  PID  AD            TUR     DURUM     CPU(tick)  CR3\n");
    thread_t *t = &g_main;
    do {
        if (t->state != T_DEAD) {
            kprintf("  %-4d %-13s %-7s %-9s %-10llu 0x%llx\n", t->id, t->name,
                    t->cr3 != g_kernel_cr3 ? "user" : "kernel", names[t->state], t->cpu_ticks, t->cr3);
        }
        t = t->next;
    } while (t != &g_main);
    irq_restore(f);
}

int thread_get_info(int index, thread_info_t *out) {
    UINT64 f = irq_save();
    int n = 0, found = 0;
    thread_t *t = &g_main;
    do {
        if (t->state != T_DEAD) {
            if (n == index) {
                out->id = t->id;
                memcpy(out->name, t->name, sizeof out->name);
                out->state = (int)t->state;
                out->is_user = t->cr3 != g_kernel_cr3;
                out->is_idle = t->is_idle;
                out->cpu_ticks = t->cpu_ticks;
                found = 1;
                break;
            }
            n++;
        }
        t = t->next;
    } while (t != &g_main);
    irq_restore(f);
    return found;
}

UINT64 thread_idle_ticks(void) { return g_idle ? g_idle->cpu_ticks : 0; }

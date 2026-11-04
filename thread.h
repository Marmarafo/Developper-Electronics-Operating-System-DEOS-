#ifndef DEOS_THREAD_H
#define DEOS_THREAD_H
#include "types.h"

// Preemptive (timer ile bolunen), round-robin thread'ler. Bir "surec", kendi adres uzayi
// (CR3) olan ve ring 3'te calisan bir thread'dir.
// Thread 0 = "shell" (kernel_main'in kendisi); "idle" hicbiri hazir degilse calisir.
void thread_init(void);

// Kernel thread'i (kernel adres uzayi). Basarisizsa -1, yoksa thread id.
int  thread_create(const char *name, void (*fn)(void *), void *arg);
// Kullanici sureci icin thread: verilen CR3'te calisir; proc = surece ozel veri (thread bitince kfree edilir).
int  thread_create_user(const char *name, void (*fn)(void *), void *arg, UINT64 cr3, void *proc);

void thread_yield(void);
void thread_sleep_ms(UINT32 ms);         // en az ms kadar uyu (10 ms cozunurluk)
void thread_exit(void);                  // mevcut thread'i bitir
int  thread_kill(int id);                // baska bir thread'i sonlandir; 0 = tamam
int  thread_count(void);                 // yasayan thread sayisi (idle haric)
void thread_dump(void);                  // 'ps' ciktisi
void thread_preempt(void);               // timer kesmesinden cagrilir (irq.c)

int   thread_alive(int id);
int   thread_current_id(void);
void *thread_current_proc(void);
void  thread_set_exit_code(int code);
int   thread_get_exit_code(int id, int *code);   // 1 = bulundu


// Sistem izleyici icin: yasayan thread'leri sirayla dolasma.
typedef struct {
    int    id;
    char   name[16];
    int    state;          // 0 READY, 1 RUNNING, 2 SLEEPING
    int    is_user, is_idle;
    UINT64 cpu_ticks;
} thread_info_t;
int    thread_get_info(int index, thread_info_t *out);   // 1 = dolduruldu, 0 = indeks disi
UINT64 thread_idle_ticks(void);

#endif

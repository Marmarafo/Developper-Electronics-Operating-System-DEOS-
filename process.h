#ifndef DEOS_PROCESS_H
#define DEOS_PROCESS_H
#include "types.h"
#include "progs.h"
#include "isr_frame.h"
#include "file.h"

#define MAX_FD    16
#define MAX_ARGS  16

// Surece ozel durum (thread'in proc alani): heap siniri, acik dosyalar, calisma dizini.
typedef struct proc {
    UINT64 heap_base, brk;
    file_t *fds[MAX_FD];
    char    cwd[128];
} proc_t;

const embedded_prog_t *prog_find(const char *name);
proc_t *process_current(void);                   // calisan surecin proc'u (kernel thread'i ise NULL)

// Genel baslatici. argv kernel belleginde olmalidir. parent != NULL ise fd'ler (fdmap ile) ondan devralinir.
// fdmap: int[3] (stdin, stdout, stderr icin parent fd numarasi; -1 = parent'in 0/1/2'si). pid | -1 (yok) | -2 (gecersiz/bellek yok)
int  process_spawn_file(const char *abs_path, int argc, char **argv, const char *cwd, proc_t *parent, const int *fdmap);
int  process_spawn_embedded(const char *name, int argc, char **argv, const char *cwd, proc_t *parent, const int *fdmap);

// Kolay sarmalayicilar (argumansiz, kok dizin)
int  process_start(const char *name, const UINT8 *elf, UINT64 size);
int  process_start_by_name(const char *name);
int  process_start_file(const char *abs_path);

void process_free(void *proc);                   // thread bitince: fd'leri kapatir, proc'u serbest birakir
long long process_sbrk(long long delta);
void process_exit(int code) __attribute__((noreturn));

// Ring 3'ten gelen exception: sureci sonlandirir, kernel yasar. Geri donmez.
void process_user_fault(isr_frame_t *frame) __attribute__((noreturn));

#endif

#ifndef DEOS_PROCESS_H
#define DEOS_PROCESS_H
#include "types.h"
#include "progs.h"
#include "isr_frame.h"

// Bir surec = kendi adres uzayi (PML4) olan, ring 3'te calisan bir thread.
const embedded_prog_t *prog_find(const char *name);

int  process_start(const char *name, const UINT8 *elf, UINT64 size);   // pid | -1
int  process_start_by_name(const char *name);                          // pid | -1 (yok) | -2 (yuklenemedi)

// Dosya sisteminden bir ELF yukleyip calistirir. pid | -1 (dosya yok) | -2 (gecersiz ELF / bellek yok)
int  process_start_file(const char *abs_path);

long long process_sbrk(long long delta);        // eski brk | -1
void process_exit(int code) __attribute__((noreturn));

// Ring 3'ten gelen exception: sureci sonlandirir, kernel yasar. Geri donmez.
void process_user_fault(isr_frame_t *frame) __attribute__((noreturn));

#endif

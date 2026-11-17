#ifndef DEOS_SYSCALL_H
#define DEOS_SYSCALL_H
#include "types.h"

// syscall/sysret mekanizmasini kurar (EFER.SCE, STAR, LSTAR, SFMASK) ve mumkunse SMEP'i acar.
void syscall_init(void);
int  syscall_smep_enabled(void);
int  syscall_smap_enabled(void);       // CR4.SMAP acik mi (CPUID.7.0:EBX[20] varsa acilir)

// Zamanlayici her thread degisiminde cagirir: syscall'in gecilecegi kernel stack'i.
void syscall_set_kstack(UINT64 kstack_top);

#endif

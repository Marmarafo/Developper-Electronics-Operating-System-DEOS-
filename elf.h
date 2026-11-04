#ifndef DEOS_ELF_H
#define DEOS_ELF_H
#include "types.h"

// ELF64 (x86-64, ET_EXEC) imajini, verilen adres uzayina (cr3) yukler.
// PT_LOAD segmentleri sayfa sayfa ayrilir, izinler (R/W/X) sayfalara yansitilir.
// Basarida 0 ve *entry = giris noktasi; hatada -1 (cagiran adres uzayini yok etmelidir).
int elf_load(const UINT8 *img, UINT64 size, UINT64 cr3, UINT64 *entry);

#endif

#ifndef DEOS_GDT_H
#define DEOS_GDT_H

// Segment secicileri: 0x08 kernel kod, 0x10 kernel veri, 0x18/0x20 kullanici
// veri/kod (henuz kullanilmiyor), 0x28 TSS.
void gdt_init(void);

// Ring 3 -> ring 0 gecislerinde (kesme/exception) CPU'nun kullanacagi kernel stack'i.
// Her thread degisiminde o thread'in kernel stack'ine ayarlanir.
void tss_set_rsp0(unsigned long long rsp0);

#endif

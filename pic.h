#ifndef DEOS_PIC_H
#define DEOS_PIC_H
#include "types.h"

void pic_init(void);
void pic_send_eoi(UINT8 irq);
void pic_send_eoi_master(void);
// O sirada ISR biti kurulu mu? (spurious IRQ ayirt etmede kullanilir)
int  pic_irq_is_real(UINT8 irq);
void pic_mask(UINT8 irq);
void pic_unmask(UINT8 irq);

#endif

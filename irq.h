#ifndef DEOS_IRQ_H
#define DEOS_IRQ_H
#include "types.h"

typedef void (*irq_handler_t)(void);

void irq_init(void);
void irq_register_handler(int irq, irq_handler_t handler);

#endif

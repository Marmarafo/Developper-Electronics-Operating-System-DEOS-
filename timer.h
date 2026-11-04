#ifndef DEOS_TIMER_H
#define DEOS_TIMER_H
#include "types.h"

void timer_init(UINT32 frequency_hz);
UINT64 timer_get_ticks(void);

#endif

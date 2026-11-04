// DEOS -- isr_stubs.S ve irq_stubs.S'in ortak stub'i tarafindan olusturulan
// yigin (stack) goruntusu. idt.c (CPU exception) ve irq.c (donanim IRQ)
// tarafindan ortaklasa kullanilir.
#ifndef DEOS_ISR_FRAME_H
#define DEOS_ISR_FRAME_H
#include "types.h"

typedef struct __attribute__((packed)) {
    UINT64 r15, r14, r13, r12, r11, r10, r9, r8;
    UINT64 rbp, rdi, rsi, rdx, rcx, rbx, rax;
    UINT64 int_no, err_code;   // int_no: isr_stubs icin vektor, irq_stubs icin IRQ (0-15)
    UINT64 rip, cs, rflags, user_rsp, ss;
} isr_frame_t;

#endif

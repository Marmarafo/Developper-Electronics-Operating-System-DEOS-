// DEOS -- Interrupt Descriptor Table (IDT) kurulumu.
// CPU'nun 0-31 arasi ayirdigi "exception" vektorlerini isr_stubs.S'e,
// donanim IRQ'larinin (PIC tarafindan 32-47'ye yeniden eslenmis) vektorlerini
// irq_stubs.S'e baglar.
#include "types.h"
#include "idt.h"
#include "isr_frame.h"

typedef struct __attribute__((packed)) {
    UINT16 offset_low;
    UINT16 selector;
    UINT8  ist;
    UINT8  type_attr;
    UINT16 offset_mid;
    UINT32 offset_high;
    UINT32 zero;
} idt_entry_t;

typedef struct __attribute__((packed)) {
    UINT16 limit;
    UINT64 base;
} idt_ptr_t;

static idt_entry_t g_idt[256];
static idt_ptr_t   g_idt_ptr;

extern void idt_flush(UINT64 idt_ptr_addr);
extern UINT64 isr_stub_table[32];   // isr_stubs.S -- CPU exception (0-31)
extern UINT64 irq_stub_table[16];   // irq_stubs.S -- donanim IRQ (32-47)
extern void kernel_panic(UINT64 vector, UINT64 error_code, UINT64 rip);
extern void process_user_fault(isr_frame_t *frame) __attribute__((noreturn));   // process.c

static void idt_set_gate(int n, UINT64 handler, UINT16 selector, UINT8 type_attr, UINT8 ist) {
    g_idt[n].offset_low  = (UINT16)(handler & 0xFFFF);
    g_idt[n].selector    = selector;
    g_idt[n].ist         = ist;
    g_idt[n].type_attr   = type_attr;
    g_idt[n].offset_mid  = (UINT16)((handler >> 16) & 0xFFFF);
    g_idt[n].offset_high = (UINT32)((handler >> 32) & 0xFFFFFFFF);
    g_idt[n].zero        = 0;
}

void idt_init(void) {
    g_idt_ptr.limit = (UINT16)(sizeof(g_idt) - 1);
    g_idt_ptr.base  = (UINT64)(UINTN)&g_idt;

    for (int i = 0; i < 256; i++) idt_set_gate(i, 0, 0, 0, 0);

    // 0x08 = kernel kod segmenti (gdt.c), 0x8E = Present,DPL0,64-bit
    // interrupt gate (girisde otomatik IF=0 yapar).
    // IST: #DF (8) -> IST1, NMI (2) -> IST2, #MC (18) -> IST3 (gdt.c'deki ayri stack'ler)
    for (int i = 0; i < 32; i++) {
        UINT8 ist = (i == 8) ? 1 : (i == 2) ? 2 : (i == 18) ? 3 : 0;
        idt_set_gate(i, isr_stub_table[i], 0x08, 0x8E, ist);
    }

    for (int i = 0; i < 16; i++)
        idt_set_gate(32 + i, irq_stub_table[i], 0x08, 0x8E, 0);

    idt_flush((UINT64)(UINTN)&g_idt_ptr);
}

// isr_stubs.S -> isr_common_stub -> buraya dusuyor (CPU exception).
// Panik ekrani (metinli) kernel.c icindeki kernel_panic ile gosterilir.
void isr_handler(isr_frame_t *frame) {
    // Ring 3'ten gelen exception (CS'nin alt 2 biti = 3): sadece o surec olur, kernel yasar.
    // (NMI / #DF / #MC bundan haric: bunlar sistem seviyesi, panik.)
    if ((frame->cs & 3) == 3 && frame->int_no != 2 && frame->int_no != 8 && frame->int_no != 18)
        process_user_fault(frame);
    kernel_panic(frame->int_no, frame->err_code, frame->rip);
}

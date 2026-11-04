// DEOS -- donanim IRQ dispatch sistemi. Her IRQ hatti (0-15) icin en
// fazla bir C fonksiyonu kaydedilebilir; irq_stubs.S'teki ortak stub
// buraya (irq_dispatch) dusup dogru handler'i cagirir, sonra PIC'e
// "isim bitti" (EOI) sinyalini gonderir.
#include "types.h"
#include "irq.h"
#include "isr_frame.h"
#include "pic.h"
#include "thread.h"

static irq_handler_t g_handlers[16];

void irq_init(void) {
    for (int i = 0; i < 16; i++) g_handlers[i] = 0;
}

void irq_register_handler(int irq, irq_handler_t handler) {
    if (irq >= 0 && irq < 16) g_handlers[irq] = handler;
}

// irq_stubs.S -> irq_common_stub -> buraya. frame->int_no burada IRQ
// numarasidir (0-15) -- isr_handler'daki CPU exception vektoru DEGIL.
void irq_dispatch(isr_frame_t *frame) {
    UINT64 irq_no = frame->int_no;
    if (irq_no < 16 && g_handlers[irq_no]) {
        g_handlers[irq_no]();
    }
    pic_send_eoi((UINT8)irq_no);
    if (irq_no == 0) thread_preempt();   // timer: zaman dilimi bitti -> siradaki thread'e gec (EOI'den SONRA!)
}

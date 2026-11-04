// DEOS -- PIT (8253/8254 Programmable Interval Timer) suruculeru.
// PIT'i istenen frekansta (Hz) kare dalga uretecek sekilde ayarlar ve
// IRQ0 uzerinden gelen her tick'te bir sayaci artiran handler'i kaydeder.
// Bu, kernel'in DONANIM KESMESIYLE calisan ilk gercek suruculugudur.
#include "types.h"
#include "timer.h"
#include "irq.h"
#include "pic.h"
#include "io.h"

#define PIT_CHANNEL0 0x40
#define PIT_COMMAND  0x43
#define PIT_BASE_FREQ 1193182u

static volatile UINT64 g_ticks = 0;

static void timer_tick_handler(void) {
    g_ticks++;
}

static void pit_configure(UINT32 frequency_hz) {
    UINT32 divisor = PIT_BASE_FREQ / frequency_hz;
    outb(PIT_COMMAND, 0x36); // kanal0, lobyte/hibyte, mod3 (kare dalga), binary
    outb(PIT_CHANNEL0, (UINT8)(divisor & 0xFF));
    outb(PIT_CHANNEL0, (UINT8)((divisor >> 8) & 0xFF));
}

void timer_init(UINT32 frequency_hz) {
    pit_configure(frequency_hz);
    irq_register_handler(0, timer_tick_handler);
    pic_unmask(0); // IRQ0 (PIT) artik CPU'ya ulasabilir
}

UINT64 timer_get_ticks(void) {
    return g_ticks;
}

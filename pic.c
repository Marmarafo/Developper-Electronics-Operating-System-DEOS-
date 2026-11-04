// DEOS -- 8259 PIC (Programmable Interrupt Controller) kurulumu.
// Varsayilan olarak IRQ0-7, vektor 8-15'e eslidir -- bu da tam olarak
// CPU exception'larimizla (0-31) CAKISIR. Bu yuzden PIC'i vektor 32'nin
// ustune "yeniden esliyoruz" (remap). Baslangicta hepsini maskeliyoruz;
// bir IRQ icin gercek bir handler yazdigimizda pic_unmask() ile aciyoruz.
#include "types.h"
#include "io.h"
#include "pic.h"

#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1

#define ICW1_INIT 0x11
#define ICW4_8086 0x01
#define PIC_EOI   0x20

static void pic_remap(UINT8 offset1, UINT8 offset2) {
    UINT8 a1 = inb(PIC1_DATA);
    UINT8 a2 = inb(PIC2_DATA);

    outb(PIC1_CMD, ICW1_INIT);
    outb(PIC2_CMD, ICW1_INIT);
    outb(PIC1_DATA, offset1); // master PIC -> vektor baslangici (32)
    outb(PIC2_DATA, offset2); // slave PIC  -> vektor baslangici (40)
    outb(PIC1_DATA, 4);       // master'a: IRQ2'de slave var
    outb(PIC2_DATA, 2);       // slave'e: kendi kimligi (cascade)
    outb(PIC1_DATA, ICW4_8086);
    outb(PIC2_DATA, ICW4_8086);

    outb(PIC1_DATA, a1);
    outb(PIC2_DATA, a2);
}

static void pic_mask_all(void) {
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
}

void pic_init(void) {
    pic_remap(0x20, 0x28); // IRQ0-7 -> 32-39, IRQ8-15 -> 40-47
    pic_mask_all();
}

// Bir IRQ isleyicisi bitince PIC'e "islem bitti" (End Of Interrupt)
// bildirilmezse o PIC bir daha kesme gondermez. irq>=8 ise hem slave
// hem master PIC'e EOI gondermek gerekir (slave, master'in arkasinda).
void pic_send_eoi(UINT8 irq) {
    if (irq >= 8) outb(PIC2_CMD, PIC_EOI);
    outb(PIC1_CMD, PIC_EOI);
}

static void pic_set_mask_bit(UINT8 irq, int set) {
    UINT16 port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    UINT8 bit = (irq < 8) ? irq : (UINT8)(irq - 8);
    UINT8 mask = inb(port);
    if (set) mask |= (UINT8)(1u << bit);
    else     mask &= (UINT8)~(1u << bit);
    outb(port, mask);
}

void pic_mask(UINT8 irq)   { pic_set_mask_bit(irq, 1); }
void pic_unmask(UINT8 irq) { pic_set_mask_bit(irq, 0); }

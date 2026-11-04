// DEOS -- PS/2 klavye suruculeru (Scancode Set 1, standart US QWERTY).
// IRQ1 uzerinden gelen her tus basimini/birakimini yakalar, ASCII'ye
// cevirir ve basit bir halka tampona (ring buffer) yazar. kernel bu
// tampondan keyboard_getchar() ile okur.
#include "types.h"
#include "keyboard.h"
#include "irq.h"
#include "pic.h"
#include "io.h"
#include "cpu.h"

#define KBD_DATA_PORT 0x60
#define KBD_BUF_SIZE  256

static char   g_buf[KBD_BUF_SIZE];
static volatile UINT32 g_head = 0, g_tail = 0;
static int    g_shift = 0, g_ctrl = 0, g_caps = 0, g_e0 = 0;
static UINT8  g_last_scancode = 0;
static UINT8  g_last_ascii = 0;

// Scancode Set 1 (make code) -> ASCII. Index = ham scancode degeri.
// 0 = bu tusun basit bir ASCII karsiligi yok (Ctrl, Alt, F-tuslari vb.).
static const char sc_ascii_lower[0x3B] = {
    [0x01]=27,
    [0x02]='1', [0x03]='2', [0x04]='3', [0x05]='4', [0x06]='5',
    [0x07]='6', [0x08]='7', [0x09]='8', [0x0A]='9', [0x0B]='0',
    [0x0C]='-', [0x0D]='=', [0x0E]='\b',
    [0x0F]='\t',
    [0x10]='q', [0x11]='w', [0x12]='e', [0x13]='r', [0x14]='t',
    [0x15]='y', [0x16]='u', [0x17]='i', [0x18]='o', [0x19]='p',
    [0x1A]='[', [0x1B]=']', [0x1C]='\n',
    [0x1E]='a', [0x1F]='s', [0x20]='d', [0x21]='f', [0x22]='g',
    [0x23]='h', [0x24]='j', [0x25]='k', [0x26]='l',
    [0x27]=';', [0x28]='\'', [0x29]='`',
    [0x2B]='\\',
    [0x2C]='z', [0x2D]='x', [0x2E]='c', [0x2F]='v', [0x30]='b',
    [0x31]='n', [0x32]='m',
    [0x33]=',', [0x34]='.', [0x35]='/',
    [0x39]=' ',
};

static const char sc_ascii_upper[0x3B] = {
    [0x01]=27,
    [0x02]='!', [0x03]='@', [0x04]='#', [0x05]='$', [0x06]='%',
    [0x07]='^', [0x08]='&', [0x09]='*', [0x0A]='(', [0x0B]=')',
    [0x0C]='_', [0x0D]='+', [0x0E]='\b',
    [0x0F]='\t',
    [0x10]='Q', [0x11]='W', [0x12]='E', [0x13]='R', [0x14]='T',
    [0x15]='Y', [0x16]='U', [0x17]='I', [0x18]='O', [0x19]='P',
    [0x1A]='{', [0x1B]='}', [0x1C]='\n',
    [0x1E]='A', [0x1F]='S', [0x20]='D', [0x21]='F', [0x22]='G',
    [0x23]='H', [0x24]='J', [0x25]='K', [0x26]='L',
    [0x27]=':', [0x28]='"', [0x29]='~',
    [0x2B]='|',
    [0x2C]='Z', [0x2D]='X', [0x2E]='C', [0x2F]='V', [0x30]='B',
    [0x31]='N', [0x32]='M',
    [0x33]='<', [0x34]='>', [0x35]='?',
    [0x39]=' ',
};

static void buf_push(char c) {
    UINT32 next = (g_head + 1) % KBD_BUF_SIZE;
    if (next == g_tail) return; // tampon dolu, yeni karakteri at
    g_buf[g_head] = c;
    g_head = next;
}

// irq.c -> irq_dispatch -> buraya (IRQ1 = klavye).
static void keyboard_irq_handler(void) {
    UINT8 st = inb(0x64);
    if (!(st & 1) || (st & 0x20)) return;   // veri yok ya da bu bayt FAREYE ait (aux): dokunma
    UINT8 sc = inb(KBD_DATA_PORT);
    g_last_scancode = sc;

    if (sc == 0xE0) { g_e0 = 1; return; }                  // genisletilmis tus oneki (ok tuslari vb.)
    int ext = g_e0;
    g_e0 = 0;

    if (sc == 0x2A || sc == 0x36) { g_shift = 1; return; }  // Shift basildi
    if (sc == 0xAA || sc == 0xB6) { g_shift = 0; return; }  // Shift birakildi
    if (sc == 0x1D) { g_ctrl = 1; return; }                 // Ctrl (sol veya E0 ile sag) basildi
    if (sc == 0x9D) { g_ctrl = 0; return; }
    if (sc == 0x3A) { g_caps = !g_caps; return; }           // Caps Lock
    if (sc & 0x80) return;                                  // diger tuslarin birakilmasi: yoksay

    if (ext) {                                              // E0 onekli tuslar
        char k = 0;
        switch (sc) {
        case 0x48: k = KEY_UP;     break;   case 0x50: k = KEY_DOWN;  break;
        case 0x4B: k = KEY_LEFT;   break;   case 0x4D: k = KEY_RIGHT; break;
        case 0x47: k = KEY_HOME;   break;   case 0x4F: k = KEY_END;   break;
        case 0x53: k = KEY_DELETE; break;   case 0x49: k = KEY_PGUP;  break;
        case 0x51: k = KEY_PGDN;   break;
        case 0x1C: k = '\n';       break;   // keypad Enter
        }
        if (k) { g_last_ascii = (UINT8)k; buf_push(k); }
        return;
    }

    if (sc < sizeof(sc_ascii_lower)) {
        int upper = g_shift;
        char base = sc_ascii_lower[sc];
        if (g_caps && base >= 'a' && base <= 'z') upper = !upper;       // Caps Lock sadece harfleri etkiler
        char c = upper ? sc_ascii_upper[sc] : base;
        if (g_ctrl && base >= 'a' && base <= 'z') c = (char)(base - 'a' + 1);   // Ctrl+harf -> 1..26
        if (c) {
            g_last_ascii = (UINT8)c;
            buf_push(c);
        }
    }
}

void keyboard_init(void) {
    g_head = g_tail = 0;
    g_shift = g_ctrl = g_caps = g_e0 = 0;
    irq_register_handler(1, keyboard_irq_handler);
    pic_unmask(1); // IRQ1 (klavye) artik CPU'ya ulasabilir
}

char keyboard_getchar(void) {
    if (g_head == g_tail) return 0;
    char c = g_buf[g_tail];
    g_tail = (g_tail + 1) % KBD_BUF_SIZE;
    return c;
}

UINT8 keyboard_last_scancode(void) { return g_last_scancode; }
UINT8 keyboard_last_ascii(void)    { return g_last_ascii; }

// USB klavye gibi baska kaynaklarin tus gondermesi icin (thread guvenli).
void keyboard_push(char c) {
    UINT64 f = irq_save();
    buf_push(c);
    irq_restore(f);
}

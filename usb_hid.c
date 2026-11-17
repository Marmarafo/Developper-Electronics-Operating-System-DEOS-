// DEOS -- USB HID (boot protokolu) klavye ve fare. Raporlar xHCI kesme uc noktasindan gelir;
// klavye raporlari mevcut klavye tamponuna (keyboard_push), fare raporlari mouse kuyruguna girer.
#include "types.h"
#include "usb.h"
#include "keyboard.h"
#include "mouse.h"
#include "heap.h"
#include "string.h"

typedef struct {
    UINT64 buf_phys;
    UINT8 *buf;
    int dci, len, is_kbd, caps, consec_err;
    UINT8 prev[8];
    UINT8 alt_shift_armed;
    UINT8 rep_key;
    UINT64 rep_next;
} hid_t;

static char hid_char(UINT8 u, int shift, int caps) {
    int eff = (shift != caps);   // etkin buyuk/kucuk durumu
    if (keyboard_get_layout() == KBD_LAYOUT_TR) {
        // TR Q: ek harfler US noktalama pozisyonlarinda (fiziksel yerlesim ayni).
        switch (u) {
        case 0x0C: return eff ? 'I'  : 0x96;   // i  -> ı / I (noktasiz i tusu)
        case 0x2F: return eff ? 0x91 : 0x90;   // [  -> ğ / Ğ
        case 0x30: return eff ? 0x93 : 0x92;   // ]  -> ü / Ü
        case 0x33: return eff ? 0x95 : 0x94;   // ;  -> ş / Ş
        case 0x34: return eff ? 0x97 : 'i';    // '  -> i / İ (noktali i tusu)
        case 0x36: return eff ? 0x99 : 0x98;   // ,  -> ö / Ö
        case 0x37: return eff ? 0x9B : 0x9A;   // .  -> ç / Ç
        default: break;
        }
    }
    if (u >= 0x04 && u <= 0x1D) { char c = (char)('a' + (u - 4)); if (shift != caps) c = (char)(c - 32); return c; }
    if (u >= 0x1E && u <= 0x27) { static const char n[] = "1234567890", s[] = "!@#$%^&*()"; return shift ? s[u - 0x1E] : n[u - 0x1E]; }
    switch (u) {
    case 0x28: return '\n';  case 0x29: return 27;   case 0x2A: return '\b';  case 0x2B: return '\t';  case 0x2C: return ' ';
    case 0x2D: return shift ? '_' : '-';  case 0x2E: return shift ? '+' : '=';
    case 0x2F: return shift ? '{' : '[';  case 0x30: return shift ? '}' : ']';  case 0x31: return shift ? '|' : '\\';
    case 0x33: return shift ? ':' : ';';  case 0x34: return shift ? '"' : '\'';  case 0x35: return shift ? '~' : '`';
    case 0x36: return shift ? '<' : ',';  case 0x37: return shift ? '>' : '.';   case 0x38: return shift ? '?' : '/';
    case 0x4F: return KEY_RIGHT;  case 0x50: return KEY_LEFT;  case 0x51: return KEY_DOWN;  case 0x52: return KEY_UP;
    case 0x4A: return KEY_HOME;   case 0x4D: return KEY_END;   case 0x4B: return KEY_PGUP;  case 0x4E: return KEY_PGDN;
    case 0x4C: return KEY_DELETE;
    case 0x54: return '/';  case 0x55: return '*';  case 0x56: return '-';  case 0x57: return '+';  case 0x58: return '\n';
    case 0x62: return '0';  case 0x63: return '.';
    default: break;
    }
    if (u >= 0x59 && u <= 0x61) return (char)('1' + (u - 0x59));       // keypad 1-9
    return 0;
}

char usb_hid_translate(UINT8 u, int shift, int caps) { return hid_char(u, shift, caps); }   // test icin

static void key_press(hid_t *h, UINT8 key, UINT8 mods, UINT64 now) {
    if (key == 0x39) { h->caps = !h->caps; return; }                    // Caps Lock
    int shift = (mods & 0x22) != 0, ctrl = (mods & 0x11) != 0;
    char c = hid_char(key, shift, h->caps);
    if (!c) return;
    if (ctrl && c >= 'a' && c <= 'z') c = (char)(c - 'a' + 1);          // Ctrl+harf -> 1..26
    else if (ctrl && c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 1);
    keyboard_push(c);
    h->rep_key = key;
    h->rep_next = now + 40;                                              // 400 ms sonra tekrar
}

static void kbd_report(hid_t *h, const UINT8 *r, UINT64 now) {
    for (int i = 2; i < 8; i++) {
        UINT8 k = r[i];
        if (k < 4) continue;
        int was = 0;
        for (int j = 2; j < 8; j++) if (h->prev[j] == k) was = 1;
        if (!was) key_press(h, k, r[0], now);
    }
    memcpy(h->prev, r, 8);
    if (h->rep_key) {                                                    // tus birakildiysa tekrari durdur
        int held = 0;
        for (int j = 2; j < 8; j++) if (h->prev[j] == h->rep_key) held = 1;
        if (!held) h->rep_key = 0;
    }
}

int usb_hid_attach(usbdev_t *d, int is_kbd, int in_dci, int maxpkt) {
    hid_t *h = kcalloc(1, sizeof *h);
    if (!h) return 0;
    h->buf_phys = usb_dma_alloc(1);
    if (!h->buf_phys) { kfree(h); return 0; }
    h->buf = (UINT8*)(UINTN)h->buf_phys;
    h->dci = in_dci; h->is_kbd = is_kbd;
    h->len = maxpkt > 8 ? 8 : maxpkt;
    d->drv = h;
    usb_arm_interrupt(d, in_dci, h->buf_phys, (UINT32)h->len);
    return 1;
}

static void kbd_mods(hid_t *h, UINT8 mods) {
    int alt_shift = ((mods & 0x44) != 0) && ((mods & 0x22) != 0);   // Alt+Shift
    if (alt_shift && !h->alt_shift_armed) keyboard_toggle_layout();
    h->alt_shift_armed = (UINT8)alt_shift;
}

void usb_hid_poll(usbdev_t *d, UINT64 now) {
    hid_t *h = d->drv;
    if (!h) return;
    xep_t *ep = &d->ep[h->dci];
    if (ep->done) {
        int code = ep->code;
        UINT32 got = (UINT32)h->len - ep->residual;
        UINT8 r[8];
        memset(r, 0, sizeof r);
        memcpy(r, h->buf, (size_t)h->len);
        ep->done = 0;
        if (code == 1 || code == 13) {
            h->consec_err = 0;
            d->reports++;
            if (h->is_kbd) { if (got >= 8) { kbd_mods(h, r[0]); kbd_report(h, r, now); } }
            else if (got >= 3) mouse_inject((signed char)r[1], (signed char)r[2], (UINT8)(r[0] & 7));
        } else {
            d->errors++;
            h->consec_err++;
        }
        if (h->consec_err < 100) usb_arm_interrupt(d, h->dci, h->buf_phys, (UINT32)h->len);   // cok hata: cihaz gitmis, birak
    }
    if (h->is_kbd && h->rep_key && now >= h->rep_next) {                 // otomatik tekrar
        key_press(h, h->rep_key, h->prev[0], now);
        h->rep_next = now + 3;
    }
}

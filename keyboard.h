#ifndef DEOS_KEYBOARD_H
#define DEOS_KEYBOARD_H
#include "types.h"

// Ozel tus kodlari (keyboard_getchar'in dondurdugu "char"lar; ASCII disi, 0x80 ustu):
#define KEY_UP     ((char)0x81)
#define KEY_DOWN   ((char)0x82)
#define KEY_LEFT   ((char)0x83)
#define KEY_RIGHT  ((char)0x84)
#define KEY_HOME   ((char)0x85)
#define KEY_END    ((char)0x86)
#define KEY_DELETE ((char)0x87)
#define KEY_PGUP   ((char)0x88)
#define KEY_PGDN   ((char)0x89)
#define KEY_ESC    ((char)27)
// Ctrl+harf: 1..26 (Ctrl+A = 1, Ctrl+S = 19, ...). Ornegin KEY_CTRL('s').
#define KEY_CTRL(c) ((char)((c) - 'a' + 1))

// Turkce harf baytlari (font 0x90-0x9B arasinda; KEY_* kodlariyla cakismaz):
// kucuk: ğ=0x90 ü=0x92 ş=0x94 ı=0x96 i=0x69(I'nin noktali hali ASCII) ö=0x98 ç=0x9A
// buyuk: Ğ=0x91 Ü=0x93 Ş=0x95 İ=0x97 Ö=0x99 Ç=0x9B
#define KBD_LAYOUT_US 0
#define KBD_LAYOUT_TR 1
int  keyboard_get_layout(void);      // aktif duzen
void keyboard_toggle_layout(void);   // US <-> TR

char keyboard_translate(int layout, UINT8 sc, int shift, int caps);   // Set 1 tarama kodu -> karakter (test edilebilir)

void keyboard_init(void);
void keyboard_push(char c);          // baska bir surucu (USB) tus gonderir
char keyboard_getchar(void);        // tampon bossa 0 doner
UINT8 keyboard_last_scancode(void); // debug/gorsel kanit icin
UINT8 keyboard_last_ascii(void);    // debug/gorsel kanit icin

#endif

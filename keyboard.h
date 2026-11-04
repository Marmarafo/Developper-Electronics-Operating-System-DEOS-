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

void keyboard_init(void);
void keyboard_push(char c);          // baska bir surucu (USB) tus gonderir
char keyboard_getchar(void);        // tampon bossa 0 doner
UINT8 keyboard_last_scancode(void); // debug/gorsel kanit icin
UINT8 keyboard_last_ascii(void);    // debug/gorsel kanit icin

#endif

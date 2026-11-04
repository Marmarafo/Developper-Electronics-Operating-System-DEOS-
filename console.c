// DEOS -- framebuffer metin konsolu. 8x16 bitmap font (font8x16.h), genis
// ekranlarda 2x buyutulur (16x32 hucre -> 1280x800'de 80x25 karakter).
#include "types.h"
#include "console.h"
#include "font8x16.h"
#include "serial.h"
#include "cpu.h"
#include "keyboard.h"

#define MAX_COLS 240
#define MAX_ROWS 100

static UINT32 *g_fb;
static UINT32 g_pitch, g_w, g_h;
static UINT32 g_scale = 1;
static UINT32 g_cw, g_ch;          // hucre boyutu (piksel)
static UINT32 g_cols, g_rows;
static UINT32 g_cx, g_cy;          // imlec (hucre koordinati)
static UINT32 g_scroll_top = 1;    // bu satirin ustu scroll etmez (durum cubugu)
static UINT32 g_fg = 0x00C0CAF5, g_bg = 0x001A1B26;
static int    g_cursor_drawn = 0;
static const console_sink_t *g_sink;

static void fill_rect(UINT32 x, UINT32 y, UINT32 w, UINT32 h, UINT32 color) {
    for (UINT32 yy = y; yy < y + h && yy < g_h; yy++) {
        UINT32 *p = g_fb + (UINT64)yy * g_pitch + x;
        for (UINT32 xx = 0; xx < w && x + xx < g_w; xx++) p[xx] = color;
    }
}

static void draw_cell(UINT32 col, UINT32 row, char ch, UINT32 fg, UINT32 bg) {
    unsigned char c = (unsigned char)ch;
    if (c >= 128) c = '?';
    const unsigned char *glyph = font8x16[c];
    UINT32 x0 = col * g_cw, y0 = row * g_ch;
    for (UINT32 gy = 0; gy < 16; gy++) {
        unsigned char bits = glyph[gy];
        for (UINT32 sy = 0; sy < g_scale; sy++) {
            UINT32 *p = g_fb + (UINT64)(y0 + gy * g_scale + sy) * g_pitch + x0;
            for (UINT32 gx = 0; gx < 8; gx++) {
                UINT32 color = (bits & (0x80u >> gx)) ? fg : bg;
                for (UINT32 sx = 0; sx < g_scale; sx++) *p++ = color;
            }
        }
    }
}

static void cursor_erase(void) {
    if (!g_cursor_drawn) return;
    fill_rect(g_cx * g_cw, g_cy * g_ch + g_ch - 2 * g_scale, g_cw, 2 * g_scale, g_bg);
    g_cursor_drawn = 0;
}

static void cursor_locked(int on) {
    if (g_sink) return;
    cursor_erase();
    if (on) {
        fill_rect(g_cx * g_cw, g_cy * g_ch + g_ch - 2 * g_scale, g_cw, 2 * g_scale, g_fg);
        g_cursor_drawn = 1;
    }
}

void console_init(BootInfo *bi) {
    g_fb    = (UINT32*)(UINTN)bi->framebuffer_base;
    g_pitch = bi->pixels_per_scanline;
    g_w     = bi->width;
    g_h     = bi->height;
    g_scale = (g_w >= 1024) ? 2 : 1;
    g_cw    = 8 * g_scale;
    g_ch    = 16 * g_scale;
    g_cols  = g_w / g_cw; if (g_cols > MAX_COLS) g_cols = MAX_COLS;
    g_rows  = g_h / g_ch; if (g_rows > MAX_ROWS) g_rows = MAX_ROWS;
    g_scroll_top = 1;
    console_full_clear(g_bg);
    g_scroll_top = 1;
    g_cx = 0; g_cy = 1;
}

static void full_clear_locked(UINT32 bg) {
    g_sink = 0;                       // panik/tam temizleme: dogrudan framebuffer'a don
    g_bg = bg;
    g_cursor_drawn = 0;
    fill_rect(0, 0, g_w, g_h, bg);
    g_scroll_top = 0;
    g_cx = 0; g_cy = 0;
}

static void clear_locked(void) {
    if (g_sink) { if (g_sink->clear) g_sink->clear(); return; }
    g_cursor_drawn = 0;
    fill_rect(0, g_scroll_top * g_ch, g_w, g_h - g_scroll_top * g_ch, g_bg);
    g_cx = 0; g_cy = g_scroll_top;
}

void console_set_color(UINT32 fg, UINT32 bg) { g_fg = fg; g_bg = bg; if (g_sink && g_sink->set_color) g_sink->set_color(fg, bg); }
UINT32 console_cols(void) { return g_sink ? g_sink->cols : g_cols; }
UINT32 console_rows(void) { return g_sink ? g_sink->rows : g_rows; }

static void scroll(void) {
    UINT32 dst_y = g_scroll_top * g_ch;
    UINT32 src_y = (g_scroll_top + 1) * g_ch;
    UINT32 lines = (g_rows - g_scroll_top - 1) * g_ch;
    for (UINT32 y = 0; y < lines; y++) {
        UINT32 *d = g_fb + (UINT64)(dst_y + y) * g_pitch;
        UINT32 *s = g_fb + (UINT64)(src_y + y) * g_pitch;
        for (UINT32 x = 0; x < g_w; x++) d[x] = s[x];
    }
    fill_rect(0, (g_rows - 1) * g_ch, g_w, g_ch, g_bg);
}

static void newline(void) {
    g_cx = 0;
    g_cy++;
    if (g_cy >= g_rows) { scroll(); g_cy = g_rows - 1; }
}

static void putc_locked(char c) {
    if (c == '\n') serial_putc('\r');   // hata ayiklama: seri porta yansit
    serial_putc(c);
    if (g_sink) { g_sink->putc(c); return; }
    cursor_erase();
    if (c == '\n')      { newline(); }
    else if (c == '\r') { g_cx = 0; }
    else if (c == '\b') {
        if (g_cx > 0) { g_cx--; draw_cell(g_cx, g_cy, ' ', g_fg, g_bg); }
    }
    else if (c == '\t') {
        UINT32 next = (g_cx / 8 + 1) * 8;
        while (g_cx < next && g_cx < g_cols) { draw_cell(g_cx, g_cy, ' ', g_fg, g_bg); g_cx++; }
        if (g_cx >= g_cols) newline();
    }
    else if (c >= 32 && c < 127) {
        draw_cell(g_cx, g_cy, c, g_fg, g_bg);
        g_cx++;
        if (g_cx >= g_cols) newline();
    }
}

void console_puts(const char *s) {
    while (*s) console_putc(*s++);
}

static void put_at_locked(UINT32 col, UINT32 row, const char *s, UINT32 fg, UINT32 bg) {
    if (g_sink) return;
    if (row >= g_rows) return;
    while (*s && col < g_cols) draw_cell(col++, row, *s++, fg, bg);
    while (col < g_cols) draw_cell(col++, row, ' ', fg, bg);
}

// Thread'ler arasi guvenlik: ekran/imlec durumu kesmeler kapaliyken degistirilir.
void console_cursor(int on)      { UINT64 f = irq_save(); cursor_locked(on); irq_restore(f); }
void console_full_clear(UINT32 bg) { UINT64 f = irq_save(); full_clear_locked(bg); irq_restore(f); }
void console_clear(void)         { UINT64 f = irq_save(); clear_locked(); irq_restore(f); }
void console_putc(char c)        { UINT64 f = irq_save(); putc_locked(c); irq_restore(f); }
void console_put_at(UINT32 col, UINT32 row, const char *s, UINT32 fg, UINT32 bg) {
    UINT64 f = irq_save(); put_at_locked(col, row, s, fg, bg); irq_restore(f);
}

void console_set_sink(const console_sink_t *sink) { UINT64 f = irq_save(); g_sink = sink; irq_restore(f); }

int console_getchar(void) {
    if (g_sink && g_sink->getchar) return g_sink->getchar();
    char c = keyboard_getchar();
    return c ? (int)(unsigned char)c : -1;
}

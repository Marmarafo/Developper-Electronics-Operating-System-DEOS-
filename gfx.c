// DEOS -- grafik katmani (arka tampon, kirpma, alfa, yazi, imlec).
// Hiz icin -O2 ile derlenir; dolgu/kopyalama "rep stosl/movsl" ile yapilir.
#include "types.h"
#include "gfx.h"
#include "pmm.h"
#include "string.h"
#include "font8x16.h"

static UINT32 *g_fb;          // ekran (write-combining)
static UINT32 *g_bb;          // arka tampon (RAM)
static int g_w, g_h, g_fb_pitch;
static int cx0, cy0, cx1, cy1;      // kirpma dikdortgeni [cx0,cx1) x [cy0,cy1)

static inline void fill32(UINT32 *p, UINT32 v, UINT64 n) {
    __asm__ __volatile__ ("rep stosl" : "+D"(p), "+c"(n) : "a"(v) : "memory");
}
static inline void copy32(UINT32 *dst, const UINT32 *src, UINT64 n) {
    __asm__ __volatile__ ("rep movsl" : "+D"(dst), "+S"(src), "+c"(n) : : "memory");
}

int gfx_init(BootInfo *bi) {
    g_fb = (UINT32*)(UINTN)bi->framebuffer_base;
    g_w = (int)bi->width;
    g_h = (int)bi->height;
    g_fb_pitch = (int)bi->pixels_per_scanline;
    UINT64 bytes = (UINT64)g_w * g_h * 4;
    UINT64 pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    UINT64 p = pmm_alloc_pages(pages);
    if (!p) return 0;
    g_bb = (UINT32*)(UINTN)p;
    memset(g_bb, 0, (size_t)bytes);
    gfx_set_clip(0, 0, g_w, g_h);
    return 1;
}

int gfx_width(void)  { return g_w; }
int gfx_height(void) { return g_h; }

void gfx_set_clip(int x, int y, int w, int h) {
    cx0 = x < 0 ? 0 : x;           cy0 = y < 0 ? 0 : y;
    cx1 = x + w > g_w ? g_w : x + w;  cy1 = y + h > g_h ? g_h : y + h;
    if (cx1 < cx0) cx1 = cx0;
    if (cy1 < cy0) cy1 = cy0;
}

void gfx_clip_intersect(int x, int y, int w, int h) {
    int nx0 = x > cx0 ? x : cx0, ny0 = y > cy0 ? y : cy0;
    int nx1 = x + w < cx1 ? x + w : cx1, ny1 = y + h < cy1 ? y + h : cy1;
    cx0 = nx0; cy0 = ny0;
    cx1 = nx1 < nx0 ? nx0 : nx1;
    cy1 = ny1 < ny0 ? ny0 : ny1;
}

void gfx_get_clip(int *x, int *y, int *w, int *h) { *x = cx0; *y = cy0; *w = cx1 - cx0; *h = cy1 - cy0; }

UINT32 gfx_mix(UINT32 a, UINT32 b, int alpha_b) {
    int ia = 255 - alpha_b;
    UINT32 r = (((a >> 16) & 255) * ia + ((b >> 16) & 255) * alpha_b) / 255;
    UINT32 g = (((a >> 8)  & 255) * ia + ((b >> 8)  & 255) * alpha_b) / 255;
    UINT32 bl = ((a & 255) * ia + (b & 255) * alpha_b) / 255;
    return (r << 16) | (g << 8) | bl;
}

void gfx_rect(int x, int y, int w, int h, UINT32 color) {
    int x0 = x > cx0 ? x : cx0, y0 = y > cy0 ? y : cy0;
    int x1 = x + w < cx1 ? x + w : cx1, y1 = y + h < cy1 ? y + h : cy1;
    if (x1 <= x0 || y1 <= y0) return;
    for (int yy = y0; yy < y1; yy++) fill32(&g_bb[yy * g_w + x0], color, (UINT64)(x1 - x0));
}

void gfx_rect_outline(int x, int y, int w, int h, UINT32 color) {
    gfx_rect(x, y, w, 1, color);
    gfx_rect(x, y + h - 1, w, 1, color);
    gfx_rect(x, y, 1, h, color);
    gfx_rect(x + w - 1, y, 1, h, color);
}

void gfx_rect_alpha(int x, int y, int w, int h, UINT32 color, int alpha) {
    int x0 = x > cx0 ? x : cx0, y0 = y > cy0 ? y : cy0;
    int x1 = x + w < cx1 ? x + w : cx1, y1 = y + h < cy1 ? y + h : cy1;
    if (x1 <= x0 || y1 <= y0) return;
    for (int yy = y0; yy < y1; yy++) {
        UINT32 *p = &g_bb[yy * g_w + x0];
        for (int xx = x0; xx < x1; xx++, p++) *p = gfx_mix(*p, color, alpha);
    }
}

void gfx_vgradient(int x, int y, int w, int h, UINT32 top, UINT32 bottom) {
    int x0 = x > cx0 ? x : cx0, x1 = x + w < cx1 ? x + w : cx1;
    if (x1 <= x0 || h <= 0) return;
    for (int yy = y > cy0 ? y : cy0; yy < (y + h < cy1 ? y + h : cy1); yy++) {
        int t = (yy - y) * 255 / (h > 1 ? h - 1 : 1);
        fill32(&g_bb[yy * g_w + x0], gfx_mix(top, bottom, t), (UINT64)(x1 - x0));
    }
}

static void draw_glyph(int x, int y, char ch, UINT32 fg, int use_bg, UINT32 bg) {
    unsigned char c = (unsigned char)ch;
    if (c >= 128) c = '?';
    const unsigned char *gl = font8x16[c];
    for (int gy = 0; gy < 16; gy++) {
        int yy = y + gy;
        if (yy < cy0 || yy >= cy1) continue;
        unsigned char bits = gl[gy];
        UINT32 *row = &g_bb[yy * g_w];
        for (int gx = 0; gx < 8; gx++) {
            int xx = x + gx;
            if (xx < cx0 || xx >= cx1) continue;
            if (bits & (0x80u >> gx)) row[xx] = fg;
            else if (use_bg) row[xx] = bg;
        }
    }
}

void gfx_char(int x, int y, char ch, UINT32 fg)                    { draw_glyph(x, y, ch, fg, 0, 0); }
void gfx_char_bg(int x, int y, char ch, UINT32 fg, UINT32 bg)      { draw_glyph(x, y, ch, fg, 1, bg); }

void gfx_text(int x, int y, const char *s, UINT32 fg) {
    for (; *s; s++, x += 8) {
        if (x >= cx1) break;
        if (x + 8 > cx0) draw_glyph(x, y, *s, fg, 0, 0);
    }
}

int gfx_text_width(const char *s) { int n = 0; while (s[n]) n++; return n * 8; }

void gfx_text_scaled(int x, int y, const char *s, UINT32 fg, int scale, int alpha) {
    for (; *s; s++, x += 8 * scale) {
        unsigned char c = (unsigned char)*s;
        if (c >= 128) c = '?';
        const unsigned char *gl = font8x16[c];
        for (int gy = 0; gy < 16; gy++) {
            unsigned char bits = gl[gy];
            for (int gx = 0; gx < 8; gx++) {
                if (!(bits & (0x80u >> gx))) continue;
                int px = x + gx * scale, py = y + gy * scale;
                if (alpha >= 255) gfx_rect(px, py, scale, scale, fg);
                else gfx_rect_alpha(px, py, scale, scale, fg, alpha);
            }
        }
    }
}

void gfx_flush(int x, int y, int w, int h) {
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w > g_w ? g_w : x + w, y1 = y + h > g_h ? g_h : y + h;
    if (x1 <= x0 || y1 <= y0) return;
    for (int yy = y0; yy < y1; yy++)
        copy32(&g_fb[yy * g_fb_pitch + x0], &g_bb[yy * g_w + x0], (UINT64)(x1 - x0));
}

// 12x17 ok: 'X' siyah kenar, '.' beyaz dolgu
static const char *g_cursor[GFX_CURSOR_H] = {
    "X           ", "XX          ", "X.X         ", "X..X        ", "X...X       ", "X....X      ",
    "X.....X     ", "X......X    ", "X.......X   ", "X........X  ", "X.....XXXXX ", "X..X..X     ",
    "X.X X..X    ", "XX  X..X    ", "X    X..X   ", "     X..X   ", "      XX    ",
};

void gfx_cursor_draw(int x, int y) {
    for (int cy = 0; cy < GFX_CURSOR_H; cy++) {
        int yy = y + cy;
        if (yy < 0 || yy >= g_h) continue;
        for (int cx = 0; cx < GFX_CURSOR_W; cx++) {
            char c = g_cursor[cy][cx];
            int xx = x + cx;
            if (c == ' ' || xx < 0 || xx >= g_w) continue;
            g_fb[yy * g_fb_pitch + xx] = (c == 'X') ? 0x00000000u : 0x00FFFFFFu;
        }
    }
}

void gfx_cursor_erase(int x, int y) { gfx_flush(x, y, GFX_CURSOR_W, GFX_CURSOR_H); }

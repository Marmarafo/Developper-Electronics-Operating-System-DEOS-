// DEOS -- pencere yoneticisi + masaustu. Bir "desktop" kernel thread'i fare ve klavye
// olaylarini isler, kirli (dirty) dikdortgeni arka tamponda yeniden cizer ve ekrana kopyalar.
// Hiz icin -O2 ile derlenir.
#include "types.h"
#include "wm.h"
#include "acpi.h"
#include "apps.h"
#include "gfx.h"
#include "mouse.h"
#include "rtc.h"
#include "keyboard.h"
#include "thread.h"
#include "timer.h"
#include "kprintf.h"
#include "string.h"
#include "cpu.h"
#include "io.h"
#include "config.h"

#define MAX_WIN     16
#define TASKBAR_H   32
#define MENU_ITEM_H 30
#define MENU_W      230
#define BTN_W       148

// ---- renk temasi ----
#define COL_DESK_TOP   0x2B3A67
#define COL_DESK_BOT   0x0C1020
#define COL_TASKBAR    0x14161F
#define COL_ACCENT     0x4F8CFF
#define COL_WIN_BG     0x1E2030
#define COL_TITLE_A1   0x3E63B8
#define COL_TITLE_A2   0x2A4585
#define COL_TITLE_I1   0x353A4D
#define COL_TITLE_I2   0x2A2E3E
#define COL_BORDER_I   0x454B63
#define COL_TEXT       0xE6E9F5
#define COL_DIM        0x9AA3BD

static window_t g_win[MAX_WIN];
static int g_z[MAX_WIN], g_nz;          // z-sirasi: alttan uste pencere indeksleri
static int g_focus = -1;

static int g_active;
static int g_sw, g_sh;
static int g_mx, g_my;                  // fare konumu
static UINT8 g_buttons;
static int g_drag = -1, g_drag_dx, g_drag_dy;
static int g_menu_open, g_menu_hover = -1;
static rtc_time_t g_time;

// kirli dikdortgen (birlesim)
static int g_dirty, g_dx0, g_dy0, g_dx1, g_dy1;

typedef struct { const char *label; void (*fn)(void); } launcher_t;
static void do_reboot(void);
static void do_shutdown(void);
static const launcher_t g_menu[] = {
    { "Terminal",         app_terminal_open },
    { "Dosyalar",         app_files_open    },
    { "Sistem Izleyici",  app_monitor_open  },
    { "Not Defteri",      app_notepad_open  },
    { "Hesap Makinesi",   app_calc_open     },
    { "Hakkinda",         app_about_open    },
    { "Yeniden Baslat",   do_reboot         },
    { "Kapat",            do_shutdown       },
};
#define MENU_N ((int)(sizeof(g_menu) / sizeof(g_menu[0])))

typedef struct { const char *label; const char *glyph; UINT32 color; void (*fn)(void); } icon_t;
static const icon_t g_icons[] = {
    { "Terminal", ">_", 0x2D3350, app_terminal_open },
    { "Dosyalar", "F",  0xB5792A, app_files_open    },
    { "Izleyici", "~",  0x1F6F54, app_monitor_open  },
    { "Not Def.", "N",  0x8A6A1F, app_notepad_open  },
    { "Hesap",    "=",  0x7A3B8C, app_calc_open     },
    { "Hakkinda", "i",  0x2F6FB0, app_about_open    },
};
#define ICON_N ((int)(sizeof(g_icons) / sizeof(g_icons[0])))
#define ICON_X 24
#define ICON_SIZE 56
#define ICON_PITCH 92

// ---------------------------------------------------------------- kirli bolge

void wm_invalidate_rect(int x, int y, int w, int h) {
    UINT64 f = irq_save();
    int x1 = x + w, y1 = y + h;
    if (!g_dirty) { g_dx0 = x; g_dy0 = y; g_dx1 = x1; g_dy1 = y1; g_dirty = 1; }
    else {
        if (x < g_dx0) g_dx0 = x;
        if (y < g_dy0) g_dy0 = y;
        if (x1 > g_dx1) g_dx1 = x1;
        if (y1 > g_dy1) g_dy1 = y1;
    }
    irq_restore(f);
}

static int take_dirty(int *x, int *y, int *w, int *h) {
    UINT64 f = irq_save();
    int d = g_dirty;
    int x0 = g_dx0, y0 = g_dy0, x1 = g_dx1, y1 = g_dy1;
    g_dirty = 0;
    irq_restore(f);
    if (!d) return 0;
    if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
    if (x1 > g_sw) x1 = g_sw;
        if (y1 > g_sh) y1 = g_sh;
    if (x1 <= x0 || y1 <= y0) return 0;
    *x = x0; *y = y0; *w = x1 - x0; *h = y1 - y0;
    return 1;
}

static void invalidate_outer(window_t *w) { wm_invalidate_rect(w->x - 1, w->y - 1, w->w + 8, w->h + 8); }   // golge dahil

void wm_invalidate_window(window_t *w) { wm_invalidate_rect(w->x + 1, w->y + WM_TITLE_H, w->w - 2, w->h - WM_TITLE_H - 1); }

// ------------------------------------------------------------------ pencereler

static int widx(window_t *w) { return (int)(w - g_win); }

static void z_remove(int idx) {
    for (int i = 0; i < g_nz; i++)
        if (g_z[i] == idx) { for (int j = i; j < g_nz - 1; j++) g_z[j] = g_z[j + 1]; g_nz--; return; }
}

static void focus_top(void) {
    g_focus = -1;
    for (int i = g_nz - 1; i >= 0; i--)
        if (!g_win[g_z[i]].minimized) { g_focus = g_z[i]; break; }
}

window_t *wm_create(const char *title, int x, int y, int cw, int ch) {
    for (int i = 0; i < MAX_WIN; i++) {
        if (g_win[i].used) continue;
        window_t *w = &g_win[i];
        memset(w, 0, sizeof *w);
        w->used = 1;
        w->w = cw + 2;
        w->h = ch + WM_TITLE_H + 1;
        w->x = x; w->y = y;
        if (w->x + w->w > g_sw) w->x = g_sw - w->w;
        if (w->y + w->h > g_sh - TASKBAR_H) w->y = g_sh - TASKBAR_H - w->h;
        if (w->x < 0) w->x = 0;
        if (w->y < 0) w->y = 0;
        w->closable = 1;
        size_t n = strlen(title);
        if (n > 31) n = 31;
        memcpy(w->title, title, n);
        g_z[g_nz++] = i;
        g_focus = i;
        invalidate_outer(w);
        return w;
    }
    return 0;
}

void wm_activate(window_t *w) {
    int idx = widx(w);
    w->minimized = 0;
    z_remove(idx);
    g_z[g_nz++] = idx;
    g_focus = idx;
    invalidate_outer(w);
    wm_invalidate_rect(0, g_sh - TASKBAR_H, g_sw, TASKBAR_H);
}

void wm_close(window_t *w) {
    if (w->on_close) w->on_close(w);
    invalidate_outer(w);
    int idx = widx(w);
    z_remove(idx);
    w->used = 0;
    if (g_focus == idx) focus_top();
    wm_invalidate_rect(0, g_sh - TASKBAR_H, g_sw, TASKBAR_H);
}

static void minimize(window_t *w) {
    w->minimized = 1;
    invalidate_outer(w);
    if (g_focus == widx(w)) focus_top();
    wm_invalidate_rect(0, g_sh - TASKBAR_H, g_sw, TASKBAR_H);
}

// ------------------------------------------------------------------- cizim

static void paint_desktop(void) {
    gfx_vgradient(0, 0, g_sw, g_sh - TASKBAR_H, COL_DESK_TOP, COL_DESK_BOT);
    gfx_text_scaled(g_sw - 4 * 8 * 7 - 40, g_sh - TASKBAR_H - 16 * 7 - 24, "DEOS", 0xFFFFFF, 7, 22);
    for (int i = 0; i < ICON_N; i++) {
        int x = ICON_X, y = 24 + i * ICON_PITCH;
        gfx_rect_alpha(x - 4, y - 4, ICON_SIZE + 8, ICON_SIZE + 22 + 8, 0x000000, 40);
        gfx_rect(x, y, ICON_SIZE, ICON_SIZE, g_icons[i].color);
        gfx_rect_outline(x, y, ICON_SIZE, ICON_SIZE, gfx_mix(g_icons[i].color, 0xFFFFFF, 90));
        int gw = (int)strlen(g_icons[i].glyph) * 16;
        gfx_text_scaled(x + (ICON_SIZE - gw) / 2, y + (ICON_SIZE - 32) / 2, g_icons[i].glyph, 0xFFFFFF, 2, 255);
        int lw = gfx_text_width(g_icons[i].label);
        gfx_text(x + (ICON_SIZE - lw) / 2, y + ICON_SIZE + 4, g_icons[i].label, COL_TEXT);
    }
}

static void paint_window(int idx) {
    window_t *w = &g_win[idx];
    int focused = (idx == g_focus);
    gfx_rect_alpha(w->x + 6, w->y + 6, w->w, w->h, 0x000000, 80);               // golge
    gfx_rect(w->x, w->y, w->w, w->h, focused ? COL_ACCENT : COL_BORDER_I);      // cerceve
    gfx_vgradient(w->x + 1, w->y + 1, w->w - 2, WM_TITLE_H - 1,
                  focused ? COL_TITLE_A1 : COL_TITLE_I1, focused ? COL_TITLE_A2 : COL_TITLE_I2);
    gfx_text(w->x + 10, w->y + 4, w->title, focused ? 0xFFFFFF : COL_DIM);
    if (w->closable) {
        gfx_rect(w->x + w->w - 24, w->y + 4, 18, 16, 0xC0392B);
        gfx_text(w->x + w->w - 24 + 5, w->y + 4, "x", 0xFFFFFF);
    }
    int mx = w->x + w->w - (w->closable ? 46 : 24);
    gfx_rect(mx, w->y + 4, 18, 16, 0x4A5068);
    gfx_text(mx + 5, w->y + 3, "_", 0xFFFFFF);

    int cx = w->x + 1, cy = w->y + WM_TITLE_H, cw = w->w - 2, ch = w->h - WM_TITLE_H - 1;
    gfx_rect(cx, cy, cw, ch, COL_WIN_BG);
    if (w->paint) {
        int sx, sy, sw, sh;
        gfx_get_clip(&sx, &sy, &sw, &sh);
        gfx_clip_intersect(cx, cy, cw, ch);
        w->paint(w, cx, cy, cw, ch);
        gfx_set_clip(sx, sy, sw, sh);
    }
}

static int btn_x(int n) { return 100 + n * BTN_W; }

static void paint_taskbar(void) {
    int ty = g_sh - TASKBAR_H;
    gfx_rect(0, ty, g_sw, TASKBAR_H, COL_TASKBAR);
    gfx_rect(0, ty, g_sw, 1, 0x30344A);
    gfx_rect(6, ty + 4, 84, 24, g_menu_open ? 0x6FA0FF : COL_ACCENT);
    gfx_text(6 + (84 - 4 * 8) / 2, ty + 8, "DEOS", 0xFFFFFF);

    int n = 0;
    for (int i = 0; i < MAX_WIN; i++) {
        if (!g_win[i].used) continue;
        int x = btn_x(n++);
        int act = (i == g_focus) && !g_win[i].minimized;
        gfx_rect(x, ty + 4, BTN_W - 8, 24, act ? 0x3A4468 : 0x252838);
        if (act) gfx_rect(x, ty + 26, BTN_W - 8, 2, COL_ACCENT);
        char t[20];
        size_t len = strlen(g_win[i].title);
        if (len > 16) len = 16;
        memcpy(t, g_win[i].title, len); t[len] = 0;
        gfx_text(x + 8, ty + 8, t, g_win[i].minimized ? COL_DIM : COL_TEXT);
    }

    char buf[32];
    ksnprintf(buf, sizeof buf, "%02d:%02d:%02d", g_time.hour, g_time.min, g_time.sec);
    gfx_text(g_sw - 8 * 8 - 14, ty + 2, buf, COL_TEXT);
    ksnprintf(buf, sizeof buf, "%02d.%02d.%04d", g_time.day, g_time.month, g_time.year);
    gfx_text(g_sw - 10 * 8 - 14, ty + 16, buf, COL_DIM);
}

static void menu_rect(int *x, int *y, int *w, int *h) {
    *w = MENU_W; *h = MENU_N * MENU_ITEM_H + 8;
    *x = 6; *y = g_sh - TASKBAR_H - *h - 4;
}

static void paint_menu(void) {
    int mx, my, mw, mh;
    menu_rect(&mx, &my, &mw, &mh);
    gfx_rect_alpha(mx + 5, my + 5, mw, mh, 0x000000, 90);
    gfx_rect(mx, my, mw, mh, 0x252838);
    gfx_rect_outline(mx, my, mw, mh, COL_ACCENT);
    for (int i = 0; i < MENU_N; i++) {
        int iy = my + 4 + i * MENU_ITEM_H;
        if (i == g_menu_hover) gfx_rect(mx + 3, iy, mw - 6, MENU_ITEM_H, COL_ACCENT);
        UINT32 col = i >= MENU_N - 2 ? 0xFF9A8C : COL_TEXT;
        if (i == MENU_N - 2) gfx_rect(mx + 8, iy - 1, mw - 16, 1, 0x454B63);
        gfx_text(mx + 16, iy + 7, g_menu[i].label, i == g_menu_hover ? 0xFFFFFF : col);
    }
}

static void repaint(int x, int y, int w, int h) {
    gfx_set_clip(x, y, w, h);
    paint_desktop();
    for (int i = 0; i < g_nz; i++) {
        window_t *win = &g_win[g_z[i]];
        if (win->minimized) continue;
        if (win->x + win->w + 8 < x || win->x > x + w || win->y + win->h + 8 < y || win->y > y + h) continue;
        paint_window(g_z[i]);
    }
    paint_taskbar();
    if (g_menu_open) paint_menu();
    gfx_flush(x, y, w, h);
}

// ------------------------------------------------------------------- girdi

static void do_reboot(void) { acpi_reboot(); }

static void do_shutdown(void) {
    acpi_poweroff();          // ACPI S5; yoksa QEMU/Bochs/VBox portlari + hlt
}

static int in_rect(int px, int py, int x, int y, int w, int h) { return px >= x && px < x + w && py >= y && py < y + h; }

static void menu_close(void) {
    if (!g_menu_open) return;
    int mx, my, mw, mh;
    menu_rect(&mx, &my, &mw, &mh);
    g_menu_open = 0; g_menu_hover = -1;
    wm_invalidate_rect(mx - 2, my - 2, mw + 10, mh + 10);
    wm_invalidate_rect(0, g_sh - TASKBAR_H, 100, TASKBAR_H);
}

static void on_press(int x, int y) {
    int ty = g_sh - TASKBAR_H;

    if (g_menu_open) {
        int mx, my, mw, mh;
        menu_rect(&mx, &my, &mw, &mh);
        if (in_rect(x, y, mx, my + 4, mw, MENU_N * MENU_ITEM_H)) {
            int i = (y - my - 4) / MENU_ITEM_H;
            menu_close();
            if (i >= 0 && i < MENU_N) g_menu[i].fn();
            return;
        }
        menu_close();
        if (in_rect(x, y, 6, ty + 4, 84, 24)) return;     // baslat dugmesi: sadece kapat
    }

    if (y >= ty) {                                         // gorev cubugu
        if (in_rect(x, y, 6, ty + 4, 84, 24)) {
            g_menu_open = 1;
            int mx, my, mw, mh; menu_rect(&mx, &my, &mw, &mh);
            wm_invalidate_rect(mx - 2, my - 2, mw + 10, mh + 10);
            wm_invalidate_rect(0, ty, 100, TASKBAR_H);
            return;
        }
        int n = 0;
        for (int i = 0; i < MAX_WIN; i++) {
            if (!g_win[i].used) continue;
            if (in_rect(x, y, btn_x(n), ty + 4, BTN_W - 8, 24)) {
                if (g_win[i].minimized || i != g_focus) wm_activate(&g_win[i]);
                else minimize(&g_win[i]);
                return;
            }
            n++;
        }
        return;
    }

    for (int k = g_nz - 1; k >= 0; k--) {                  // pencereler (ustten alta)
        int i = g_z[k];
        window_t *w = &g_win[i];
        if (w->minimized || !in_rect(x, y, w->x, w->y, w->w, w->h)) continue;
        if (i != g_focus || k != g_nz - 1) wm_activate(w);
        if (y < w->y + WM_TITLE_H) {                       // baslik cubugu
            if (w->closable && in_rect(x, y, w->x + w->w - 24, w->y + 4, 18, 16)) { wm_close(w); return; }
            if (in_rect(x, y, w->x + w->w - (w->closable ? 46 : 24), w->y + 4, 18, 16)) { minimize(w); return; }
            g_drag = i; g_drag_dx = x - w->x; g_drag_dy = y - w->y;
        } else if (w->on_click) {
            w->on_click(w, x - (w->x + 1), y - (w->y + WM_TITLE_H), g_buttons);
        }
        return;
    }

    for (int i = 0; i < ICON_N; i++)                       // masaustu simgeleri
        if (in_rect(x, y, ICON_X - 4, 24 + i * ICON_PITCH - 4, ICON_SIZE + 8, ICON_SIZE + 30)) { g_icons[i].fn(); return; }
}

static void on_move(int x, int y) {
    if (g_drag >= 0) {
        window_t *w = &g_win[g_drag];
        int nx = x - g_drag_dx, ny = y - g_drag_dy;
        if (ny < 0) ny = 0;
        if (ny > g_sh - TASKBAR_H - WM_TITLE_H) ny = g_sh - TASKBAR_H - WM_TITLE_H;
        if (nx < 60 - w->w) nx = 60 - w->w;
        if (nx > g_sw - 60) nx = g_sw - 60;
        if (nx != w->x || ny != w->y) {
            invalidate_outer(w);
            w->x = nx; w->y = ny;
            invalidate_outer(w);
        }
    }
    if (g_menu_open) {
        int mx, my, mw, mh;
        menu_rect(&mx, &my, &mw, &mh);
        int hv = in_rect(x, y, mx, my + 4, mw, MENU_N * MENU_ITEM_H) ? (y - my - 4) / MENU_ITEM_H : -1;
        if (hv != g_menu_hover) { g_menu_hover = hv; wm_invalidate_rect(mx, my, mw, mh); }
    }
}

static void handle_mouse(const mouse_event_t *ev) {
    int ox = g_mx, oy = g_my;
    g_mx += ev->dx; g_my += ev->dy;
    if (g_mx < 0) g_mx = 0;
        if (g_my < 0) g_my = 0;
    if (g_mx >= g_sw) g_mx = g_sw - 1;
        if (g_my >= g_sh) g_my = g_sh - 1;

    UINT8 prev = g_buttons;
    g_buttons = ev->buttons;
    on_move(g_mx, g_my);
    if ((g_buttons & 1) && !(prev & 1)) on_press(g_mx, g_my);
    if (!(g_buttons & 1) && (prev & 1)) g_drag = -1;

    if (ox != g_mx || oy != g_my) {   // imleci hemen kaydir (yeniden cizim beklemeden)
        gfx_cursor_erase(ox, oy);
        gfx_cursor_draw(g_mx, g_my);
    }
}

static void send_key(char c) {
    if (g_menu_open && c == 27) { menu_close(); return; }
    if (g_focus >= 0 && g_win[g_focus].on_key) g_win[g_focus].on_key(&g_win[g_focus], c);
}

// ------------------------------------------------------------ olay thread'i

static void wm_thread(void *arg) {
    (void)arg;
    UINT64 last_sec = ~0ULL;
    gfx_cursor_draw(g_mx, g_my);
    for (;;) {
        UINT64 now = timer_get_ticks();

        mouse_event_t ev;
        int guard = 0;
        while (guard++ < 64 && mouse_poll(&ev)) handle_mouse(&ev);

        for (int n = 0; n < 16; n++) {
            char c = keyboard_getchar();
            if (!c) break;
            send_key(c);
        }

        if (now / 100 != last_sec) {                       // saniyede bir: saat
            last_sec = now / 100;
            rtc_get(&g_time);
            wm_invalidate_rect(g_sw - 160, g_sh - TASKBAR_H, 160, TASKBAR_H);
        }

        for (int i = 0; i < MAX_WIN; i++) {
            window_t *w = &g_win[i];
            if (!w->used || !w->on_tick || !w->tick_interval || now < w->next_tick) continue;
            w->next_tick = now + (UINT64)w->tick_interval;
            if (!w->minimized && w->on_tick(w, now)) wm_invalidate_window(w);
        }

        int x, y, w, h;
        if (take_dirty(&x, &y, &w, &h)) {
            repaint(x, y, w, h);
            gfx_cursor_draw(g_mx, g_my);
        }
        thread_sleep_ms(10);
    }
}

int wm_active(void) { return g_active; }

int wm_start(BootInfo *bi) {
    if (!gfx_init(bi)) return 0;
    g_sw = gfx_width(); g_sh = gfx_height();
    int mouse_ok = mouse_init();
    g_mx = g_sw / 2; g_my = g_sh / 2;
    rtc_get(&g_time);

    g_active = 1;
    wm_invalidate_rect(0, 0, g_sw, g_sh);
    app_terminal_open();
    if (thread_create("desktop", wm_thread, 0) < 0) { g_active = 0; return 0; }
    (void)mouse_ok;
    return 1;
}

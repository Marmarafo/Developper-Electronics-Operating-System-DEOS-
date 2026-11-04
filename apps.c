// DEOS -- masaustu uygulamalari: Terminal, Sistem Izleyici, Not Defteri, Hesap Makinesi, Hakkinda.
// Hepsi wm.c'nin pencere API'sini kullanir. Hiz icin -O2 ile derlenir.
#include "types.h"
#include "apps.h"
#include "wm.h"
#include "gfx.h"
#include "console.h"
#include "timer.h"
#include "thread.h"
#include "pmm.h"
#include "heap.h"
#include "kprintf.h"
#include "string.h"
#include "config.h"
#include "cpu.h"
#include "blk.h"
#include "fs.h"
#include "process.h"
#include "keyboard.h"

#define COL_TEXT 0xE6E9F5
#define COL_DIM  0x9AA3BD
#define COL_ACC  0x4F8CFF

// ======================================================================= TERMINAL
// 80x24 karakter izgarasi. Konsol ciktisi (kprintf, kullanici programlarinin write()'i)
// buraya yonlendirilir; klavye tuslari bir giris halkasina yazilir, shell oradan okur.

#define T_COLS 80
#define T_ROWS 24
#define T_PAD  4

static window_t *g_term;
static char     t_ch[T_ROWS][T_COLS];
static UINT32   t_fg[T_ROWS][T_COLS];
static int      t_cx, t_cy;
static UINT32   t_cur_fg = 0x00D7DAE8;
static volatile int t_dirty;
static int      t_phase;
static char     t_in[256];
static volatile UINT32 t_ih, t_it;

static void term_scroll(void) {
    for (int r = 1; r < T_ROWS; r++) {
        memcpy(t_ch[r - 1], t_ch[r], T_COLS);
        memcpy(t_fg[r - 1], t_fg[r], sizeof t_fg[r]);
    }
    memset(t_ch[T_ROWS - 1], ' ', T_COLS);
}

static void term_newline(void) {
    t_cx = 0;
    if (++t_cy >= T_ROWS) { term_scroll(); t_cy = T_ROWS - 1; }
}

static void term_putc(char c) {
    if (c == '\n')      term_newline();
    else if (c == '\r') t_cx = 0;
    else if (c == '\b') { if (t_cx > 0) { t_cx--; t_ch[t_cy][t_cx] = ' '; } }
    else if (c == '\t') { int next = (t_cx / 8 + 1) * 8; while (t_cx < next && t_cx < T_COLS) { t_ch[t_cy][t_cx] = ' '; t_cx++; } if (t_cx >= T_COLS) term_newline(); }
    else if (c >= 32 && c < 127) {
        t_ch[t_cy][t_cx] = c;
        t_fg[t_cy][t_cx] = t_cur_fg;
        if (++t_cx >= T_COLS) term_newline();
    }
    t_dirty = 1;
}

static void term_set_color(UINT32 fg, UINT32 bg) { (void)bg; t_cur_fg = fg; }

static void term_clear(void) {
    memset(t_ch, ' ', sizeof t_ch);
    t_cx = 0; t_cy = 0;
    t_dirty = 1;
}

static int term_getchar(void) {
    if (t_ih == t_it) return -1;
    char c = t_in[t_it];
    t_it = (t_it + 1) % sizeof t_in;
    return (unsigned char)c;
}

static const console_sink_t g_term_sink = { term_putc, term_set_color, term_clear, term_getchar, T_COLS, T_ROWS };

static void term_paint(window_t *w, int cx, int cy, int cw, int ch) {
    (void)w;
    gfx_rect(cx, cy, cw, ch, 0x0D0F1A);
    for (int r = 0; r < T_ROWS; r++)
        for (int c = 0; c < T_COLS; c++)
            if (t_ch[r][c] != ' ') gfx_char(cx + T_PAD + c * 8, cy + T_PAD + r * 16, t_ch[r][c], t_fg[r][c]);
    if (t_phase) gfx_rect(cx + T_PAD + t_cx * 8, cy + T_PAD + t_cy * 16 + 14, 8, 2, 0x00D7DAE8);
}

static void term_key(window_t *w, char c) {
    (void)w;
    UINT32 next = (t_ih + 1) % sizeof t_in;
    if (next == t_it) return;
    t_in[t_ih] = c;
    t_ih = next;
}

static int term_tick(window_t *w, UINT64 ticks) {
    (void)w;
    int ph = (int)((ticks / 50) & 1);
    if (t_dirty || ph != t_phase) { t_dirty = 0; t_phase = ph; return 1; }
    return 0;
}

void app_terminal_open(void) {
    if (g_term) { wm_activate(g_term); return; }
    memset(t_ch, ' ', sizeof t_ch);
    g_term = wm_create("Terminal - deos shell", 150, 36, T_COLS * 8 + T_PAD * 2, T_ROWS * 16 + T_PAD * 2);
    if (!g_term) return;
    g_term->closable = 0;                 // shell bu pencerede yasar
    g_term->paint = term_paint;
    g_term->on_key = term_key;
    g_term->on_tick = term_tick;
    g_term->tick_interval = 1;
    console_set_sink(&g_term_sink);
}

// ================================================================ SISTEM IZLEYICI

#define MON_W 520
#define MON_H 360
#define HIST  60

static window_t *g_mon;
static UINT8  m_hist[HIST];
static int    m_pos, m_filled;
static UINT64 m_last_idle, m_last_tick;

static int mon_tick(window_t *w, UINT64 ticks) {
    (void)w;
    UINT64 idle = thread_idle_ticks();
    UINT64 dt = ticks - m_last_tick, di = idle - m_last_idle;
    if (m_last_tick && dt) {
        int load = 100 - (int)(di * 100 / dt);
        if (load < 0) load = 0;
        if (load > 100) load = 100;
        m_hist[m_pos] = (UINT8)load;
        m_pos = (m_pos + 1) % HIST;
        if (m_filled < HIST) m_filled++;
    }
    m_last_tick = ticks; m_last_idle = idle;
    return 1;
}

static void bar(int x, int y, int w, int h, UINT64 used, UINT64 total, UINT32 color) {
    gfx_rect(x, y, w, h, 0x14161F);
    if (total) gfx_rect(x, y, (int)((UINT64)w * used / total), h, color);
    gfx_rect_outline(x, y, w, h, 0x454B63);
}

static void mon_paint(window_t *w, int cx, int cy, int cw, int ch) {
    (void)w; (void)cw; (void)ch;
    char buf[96];
    int x = cx + 12, y = cy + 10;

    int last = m_filled ? m_hist[(m_pos + HIST - 1) % HIST] : 0;
    ksnprintf(buf, sizeof buf, "CPU kullanimi: %d%%", last);
    gfx_text(x, y, buf, COL_TEXT);
    UINT64 s = timer_get_ticks() / 100;
    ksnprintf(buf, sizeof buf, "uptime %02llu:%02llu:%02llu", s / 3600, (s / 60) % 60, s % 60);
    gfx_text(cx + MON_W - 12 - gfx_text_width(buf) - 4, y, buf, COL_DIM);

    int gx = x, gy = y + 24, gw = HIST * 8, gh = 80;
    gfx_rect(gx, gy, gw, gh, 0x14161F);
    for (int q = 1; q < 4; q++) gfx_rect(gx, gy + gh * q / 4, gw, 1, 0x2A2E3E);
    for (int i = 0; i < m_filled; i++) {
        int v = m_hist[(m_pos + HIST - m_filled + i) % HIST];
        int bh = v * (gh - 2) / 100;
        UINT32 c = v < 50 ? 0x3FB97A : (v < 80 ? 0xE0AF68 : 0xE0605E);
        gfx_rect(gx + 1 + i * 8, gy + gh - 1 - bh, 7, bh, c);
    }
    gfx_rect_outline(gx, gy, gw, gh, 0x454B63);

    pmm_stats_t ps; pmm_get_stats(&ps);
    UINT64 used_mb = (ps.usable_pages - ps.free_pages) * 4 / 1024, tot_mb = ps.usable_pages * 4 / 1024;
    y = gy + gh + 14;
    ksnprintf(buf, sizeof buf, "Fiziksel bellek: %llu / %llu MB", used_mb, tot_mb);
    gfx_text(x, y, buf, COL_TEXT);
    bar(x, y + 20, gw, 14, ps.usable_pages - ps.free_pages, ps.usable_pages, 0x4F8CFF);

    heap_stats_t hs; heap_get_stats(&hs);
    y += 46;
    ksnprintf(buf, sizeof buf, "Kernel heap: %llu / %llu KB", hs.used_bytes / 1024, hs.total_bytes / 1024);
    gfx_text(x, y, buf, COL_TEXT);
    bar(x, y + 20, gw, 14, hs.used_bytes, hs.total_bytes, 0x9B6FE0);

    y += 48;
    gfx_text(x, y, "PID  AD            TUR     DURUM     CPU(tick)", COL_ACC);
    gfx_rect(x, y + 18, gw, 1, 0x454B63);
    static const char *st[] = { "READY", "RUNNING", "SLEEP", "DEAD" };
    thread_info_t ti;
    for (int i = 0; i < 7 && thread_get_info(i, &ti); i++) {
        ksnprintf(buf, sizeof buf, "%-4d %-13s %-7s %-9s %llu", ti.id, ti.name, ti.is_user ? "user" : "kernel", st[ti.state], ti.cpu_ticks);
        gfx_text(x, y + 24 + i * 16, buf, ti.is_user ? 0x7FD6A0 : COL_TEXT);
    }
}

static void mon_close(window_t *w) { (void)w; g_mon = 0; }

void app_monitor_open(void) {
    if (g_mon) { wm_activate(g_mon); return; }
    g_mon = wm_create("Sistem Izleyici", 420, 90, MON_W, MON_H);
    if (!g_mon) return;
    g_mon->paint = mon_paint;
    g_mon->on_tick = mon_tick;
    g_mon->tick_interval = 100;
    g_mon->on_close = mon_close;
    m_pos = m_filled = 0; m_last_tick = 0;
}

// ================================================================== NOT DEFTERI
// Gercek bir metin editoru: 64 KB'a kadar dosya, imlec tuslari, kaydirma, Ctrl+S / Ctrl+O / Ctrl+N.

#define NP_COLS 76
#define NP_ROWS 24
#define NP_MAX  65536

static window_t *g_note;
static char    *np_buf;
static UINT32   np_len, np_cur;
static int      np_srow, np_scol;
static char     np_path[160];
static int      np_modified, np_mode, np_warn, np_phase;     // np_mode: 0 duzenle, 1 "ac" adi, 2 "kaydet" adi
static char     np_in[128];
static int      np_inl;
static char     np_msg[80];
static UINT64   np_msg_until;

static void np_set_msg(const char *m) {
    size_t n = strlen(m); if (n > 79) n = 79;
    memcpy(np_msg, m, n); np_msg[n] = 0;
    np_msg_until = timer_get_ticks() + 300;
}

static UINT32 np_ls(UINT32 i) { while (i > 0 && np_buf[i - 1] != '\n') i--; return i; }
static UINT32 np_le(UINT32 i) { while (i < np_len && np_buf[i] != '\n') i++; return i; }

static void np_pos(UINT32 *row, UINT32 *col) {
    UINT32 r = 0, ls = 0;
    for (UINT32 i = 0; i < np_cur; i++) if (np_buf[i] == '\n') { r++; ls = i + 1; }
    *row = r; *col = np_cur - ls;
}

static void np_view(void) {
    UINT32 r, c;
    np_pos(&r, &c);
    if ((int)r < np_srow) np_srow = (int)r;
    if ((int)r >= np_srow + NP_ROWS) np_srow = (int)r - NP_ROWS + 1;
    if ((int)c < np_scol) np_scol = (int)c;
    if ((int)c >= np_scol + NP_COLS) np_scol = (int)c - NP_COLS + 1;
}

static void np_title(void) {
    if (!g_note) return;
    const char *base = np_path[0] ? strrchr(np_path, '/') : 0;
    base = base ? base + 1 : (np_path[0] ? np_path : "Adsiz");
    ksnprintf(g_note->title, sizeof g_note->title, "Not Defteri - %.16s%s", base, np_modified ? " *" : "");
}

static void np_insert(char c) {
    if (np_len >= NP_MAX - 1) return;
    memmove(np_buf + np_cur + 1, np_buf + np_cur, np_len - np_cur);
    np_buf[np_cur++] = c; np_len++; np_modified = 1;
}
static void np_delete_at(UINT32 i) { memmove(np_buf + i, np_buf + i + 1, np_len - i - 1); np_len--; np_modified = 1; }

static void np_up(void) {
    UINT32 ls = np_ls(np_cur), col = np_cur - ls;
    if (ls == 0) return;
    UINT32 pls = np_ls(ls - 1), plen = ls - 1 - pls;
    np_cur = pls + (col < plen ? col : plen);
}
static void np_down(void) {
    UINT32 le = np_le(np_cur);
    if (le >= np_len) return;
    UINT32 col = np_cur - np_ls(np_cur), nls = le + 1, nle = np_le(nls), nlen = nle - nls;
    np_cur = nls + (col < nlen ? col : nlen);
}

static void np_new(void) {
    np_len = np_cur = 0; np_srow = np_scol = 0; np_path[0] = 0; np_modified = 0;
    np_title();
}

static void np_save_to(const char *path) {
    int r = fs_write_file(path, np_buf, np_len);
    if (r == 0) {
        size_t n = strlen(path); if (n > 159) n = 159;
        memcpy(np_path, path, n); np_path[n] = 0;
        np_modified = 0;
        char m[80]; ksnprintf(m, sizeof m, "Kaydedildi: %u bayt", np_len);
        np_set_msg(m);
    } else {
        char m[80]; ksnprintf(m, sizeof m, "Kaydedilemedi: %s", fs_strerror(r));
        np_set_msg(m);
    }
    np_title();
}

static void np_load_from(const char *path) {
    UINT64 size = 0;
    char *d = fs_read_all(path, &size);
    if (!d) { np_set_msg("Dosya acilamadi"); return; }
    if (size > NP_MAX - 1) { kfree(d); np_set_msg("Dosya cok buyuk (en fazla 64 KB)"); return; }
    memcpy(np_buf, d, (size_t)size);
    for (UINT64 i = 0; i < size; i++) if (np_buf[i] == '\r') np_buf[i] = ' ';    // CR'yi yoksay
    np_len = (UINT32)size; np_cur = 0; np_srow = np_scol = 0; np_modified = 0;
    size_t n = strlen(path); if (n > 159) n = 159;
    memcpy(np_path, path, n); np_path[n] = 0;
    kfree(d);
    np_title();
}

static void np_prompt_done(void) {
    np_in[np_inl] = 0;
    int mode = np_mode;
    np_mode = 0;
    if (!np_inl) return;
    char full[160];
    fs_normalize("/", np_in, full, sizeof full);
    if (mode == 1) np_load_from(full); else np_save_to(full);
}

static void np_key(window_t *w, char c) {
    (void)w;
    if (np_mode) {                                                   // dosya adi girisi
        if (c == '\n') np_prompt_done();
        else if (c == KEY_ESC) np_mode = 0;
        else if (c == '\b') { if (np_inl) np_inl--; }
        else if (c >= 32 && c < 127 && np_inl < 120) np_in[np_inl++] = c;
        wm_invalidate_window(g_note);
        return;
    }
    int was_warn = np_warn;
    np_warn = 0;
    switch (c) {
    case KEY_LEFT:   if (np_cur) np_cur--; break;
    case KEY_RIGHT:  if (np_cur < np_len) np_cur++; break;
    case KEY_UP:     np_up(); break;
    case KEY_DOWN:   np_down(); break;
    case KEY_HOME:   np_cur = np_ls(np_cur); break;
    case KEY_END:    np_cur = np_le(np_cur); break;
    case KEY_PGUP:   for (int i = 0; i < NP_ROWS - 1; i++) np_up(); break;
    case KEY_PGDN:   for (int i = 0; i < NP_ROWS - 1; i++) np_down(); break;
    case KEY_DELETE: if (np_cur < np_len) np_delete_at(np_cur); break;
    case '\b':       if (np_cur) { np_delete_at(np_cur - 1); np_cur--; } break;
    case '\n':       np_insert('\n'); break;
    case '\t':       for (int i = 0; i < 4; i++) np_insert(' '); break;
    case 19:                                                         // Ctrl+S: kaydet
        if (np_path[0]) np_save_to(np_path);
        else { np_mode = 2; np_inl = 0; }
        break;
    case 15:                                                         // Ctrl+O: ac
    case 14:                                                         // Ctrl+N: yeni
        if (np_modified && !was_warn) { np_warn = 1; np_set_msg("Kaydedilmemis degisiklik var! Devam icin tekrar basin"); break; }
        if (c == 14) np_new(); else { np_mode = 1; np_inl = 0; }
        break;
    default:
        if (c >= 32 && c < 127) np_insert(c);
        break;
    }
    np_title();
    np_view();
    wm_invalidate_window(g_note);
}

static void np_paint(window_t *w, int cx, int cy, int cw, int ch) {
    (void)w;
    gfx_rect(cx, cy, cw, ch - 22, 0xF7F5EC);
    UINT32 i = 0;
    for (int row = 0; row < np_srow && i < np_len; ) { if (np_buf[i] == '\n') row++; i++; }
    for (int r = 0; r < NP_ROWS; r++) {
        for (int k = 0; k < np_scol && i < np_len && np_buf[i] != '\n'; k++) i++;
        for (int c = 0; c < NP_COLS && i < np_len && np_buf[i] != '\n'; c++, i++)
            if (np_buf[i] != ' ') gfx_char(cx + 8 + c * 8, cy + 6 + r * 16, np_buf[i], 0x1A1B26);
        while (i < np_len && np_buf[i] != '\n') i++;
        if (i >= np_len) break;
        i++;
    }
    UINT32 cr, cc;
    np_pos(&cr, &cc);
    if (!np_mode && np_phase && (int)cr >= np_srow && (int)cr < np_srow + NP_ROWS && (int)cc >= np_scol && (int)cc < np_scol + NP_COLS)
        gfx_rect(cx + 8 + ((int)cc - np_scol) * 8, cy + 6 + ((int)cr - np_srow) * 16, 2, 16, 0x1A1B26);

    gfx_rect(cx, cy + ch - 22, cw, 22, 0x252838);
    char b[200];
    if (np_mode) {
        ksnprintf(b, sizeof b, "%s: %s_    (Enter: tamam, Esc: iptal)", np_mode == 1 ? "Acilacak dosya" : "Kaydet - dosya adi", np_in);
        gfx_text(cx + 8, cy + ch - 20, b, 0xFFD27A);
    } else if (np_msg[0] && timer_get_ticks() < np_msg_until) {
        gfx_text(cx + 8, cy + ch - 20, np_msg, 0xFFD27A);
    } else {
        ksnprintf(b, sizeof b, "%s%s   Satir %u, Sutun %u   |  Ctrl+S Kaydet  Ctrl+O Ac  Ctrl+N Yeni",
                  np_path[0] ? np_path : "Adsiz", np_modified ? "*" : "", cr + 1, cc + 1);
        gfx_text(cx + 8, cy + ch - 20, b, COL_DIM);
    }
}

static int np_tick(window_t *w, UINT64 t) { (void)w; int p = (int)((t / 50) & 1); int r = p != np_phase; np_phase = p; return r || (np_msg[0] && t == np_msg_until); }
static void np_close(window_t *w) { (void)w; g_note = 0; }

void app_notepad_open_file(const char *path) {
    if (!np_buf) { np_buf = kmalloc(NP_MAX); if (!np_buf) return; np_len = 0; }
    if (g_note) wm_activate(g_note);
    else {
        g_note = wm_create("Not Defteri", 300, 120, NP_COLS * 8 + 16, NP_ROWS * 16 + 12 + 22);
        if (!g_note) return;
        g_note->paint = np_paint; g_note->on_key = np_key; g_note->on_tick = np_tick;
        g_note->tick_interval = 25; g_note->on_close = np_close;
        np_mode = 0; np_warn = 0; np_msg[0] = 0;
    }
    if (path) np_load_from(path); else if (!np_len && !np_path[0]) np_new();
    np_title();
    wm_invalidate_window(g_note);
}

void app_notepad_open(void) { app_notepad_open_file(0); }

// ============================================================== DOSYA YONETICISI

#define FM_W     600
#define FM_H     420
#define FM_LIST_Y 84
#define FM_ROW_H 20
#define FM_ROWS  ((FM_H - FM_LIST_Y - 26) / FM_ROW_H)
#define FM_MAXE  200

typedef struct { char name[48]; int is_dir; UINT32 size; } fm_ent_t;

static window_t *g_fm;
static char      fm_path[160] = "/";
static fm_ent_t *fm_e;
static int       fm_n, fm_sel, fm_top, fm_mode, fm_inl, fm_confirm = -1;     // fm_mode: 0, 1 yeni klasor, 2 yeni dosya, 3 yeniden adlandir
static char      fm_in[64];
static char      fm_msg[80];
static UINT64    fm_msg_until, fm_last_click;
static int       fm_last_row = -1;

static const struct { const char *label; int x, w; } fm_btn[] = {
    { "Yukari", 6, 64 }, { "Yeni Klasor", 76, 104 }, { "Yeni Dosya", 186, 96 }, { "Sil", 288, 40 }, { "Ad Degistir", 334, 104 },
};

static void fm_set_msg(const char *m) {
    size_t n = strlen(m); if (n > 79) n = 79;
    memcpy(fm_msg, m, n); fm_msg[n] = 0;
    fm_msg_until = timer_get_ticks() + 300;
}

static void fm_reload(void) {
    if (!fm_e) fm_e = kmalloc(sizeof(fm_ent_t) * FM_MAXE);
    fm_n = 0;
    if (!fm_e) return;
    fs_dirent_t e;
    for (int i = 0; fm_n < FM_MAXE && fs_readdir(fm_path, i, &e) == 0; i++) {
        fm_ent_t *d = &fm_e[fm_n++];
        size_t n = strlen(e.name); if (n > 47) n = 47;
        memcpy(d->name, e.name, n); d->name[n] = 0;
        d->is_dir = e.is_dir; d->size = e.size;
    }
    for (int i = 1; i < fm_n; i++) {                                 // klasorler once, sonra ada gore
        fm_ent_t t = fm_e[i];
        int j = i - 1;
        while (j >= 0 && ((fm_e[j].is_dir < t.is_dir) || (fm_e[j].is_dir == t.is_dir && strcasecmp(fm_e[j].name, t.name) > 0))) { fm_e[j + 1] = fm_e[j]; j--; }
        fm_e[j + 1] = t;
    }
    if (fm_sel >= fm_n) fm_sel = fm_n - 1;
    if (fm_sel < 0) fm_sel = 0;
    fm_top = 0;
    fm_confirm = -1;
}

static void fm_fit(void) {
    if (fm_sel < fm_top) fm_top = fm_sel;
    if (fm_sel >= fm_top + FM_ROWS) fm_top = fm_sel - FM_ROWS + 1;
}

static void fm_cd(const char *name) {
    char np[160];
    fs_normalize(fm_path, name, np, sizeof np);
    size_t n = strlen(np); if (n > 159) n = 159;
    memcpy(fm_path, np, n); fm_path[n] = 0;
    fm_sel = 0; fm_top = 0;
    fm_reload();
}

static int has_ext(const char *name, const char *ext) {
    size_t n = strlen(name), e = strlen(ext);
    return n > e && !strcasecmp(name + n - e, ext);
}

static void fm_open_sel(void) {
    if (fm_sel < 0 || fm_sel >= fm_n) return;
    fm_ent_t *e = &fm_e[fm_sel];
    if (e->is_dir) { fm_cd(e->name); return; }
    char p[160];
    fs_normalize(fm_path, e->name, p, sizeof p);
    if (has_ext(e->name, ".elf")) {
        int pid = process_start_file(p);
        char m[80];
        if (pid > 0) ksnprintf(m, sizeof m, "Program baslatildi (pid %d); cikti terminalde", pid);
        else         ksnprintf(m, sizeof m, "Program calistirilamadi");
        fm_set_msg(m);
    } else {
        app_notepad_open_file(p);
    }
}

static void fm_delete_sel(void) {
    if (fm_sel < 0 || fm_sel >= fm_n) return;
    if (fm_confirm != fm_sel) { fm_confirm = fm_sel; fm_set_msg("Silmek icin tekrar basin (Sil / Delete)"); return; }
    char p[160];
    fs_normalize(fm_path, fm_e[fm_sel].name, p, sizeof p);
    int r = fs_remove(p);
    char m[80];
    if (r == 0) ksnprintf(m, sizeof m, "Silindi: %.40s", fm_e[fm_sel].name);
    else        ksnprintf(m, sizeof m, "Silinemedi: %s", fs_strerror(r));
    fm_set_msg(m);
    fm_reload();
}

static void fm_prompt_done(void) {
    fm_in[fm_inl] = 0;
    int mode = fm_mode;
    fm_mode = 0;
    if (!fm_inl) return;
    char p[160], m[80];
    fs_normalize(fm_path, fm_in, p, sizeof p);
    int r;
    if (mode == 3) {
        char old[160];
        fs_normalize(fm_path, fm_e[fm_sel].name, old, sizeof old);
        r = fs_rename(old, p);
    } else {
        r = fs_create(p, mode == 1);
    }
    if (r == 0) { ksnprintf(m, sizeof m, "Tamam"); } else ksnprintf(m, sizeof m, "Hata: %s", fs_strerror(r));
    fm_set_msg(m);
    fm_reload();
}

static void fm_up(void) { fm_cd(".."); }

static void fm_action(int b) {
    switch (b) {
    case 0: fm_up(); break;
    case 1: fm_mode = 1; fm_inl = 0; break;
    case 2: fm_mode = 2; fm_inl = 0; break;
    case 3: fm_delete_sel(); break;
    case 4:
        if (fm_n > 0) { fm_mode = 3; fm_inl = (int)strlen(fm_e[fm_sel].name); memcpy(fm_in, fm_e[fm_sel].name, (size_t)fm_inl); }
        break;
    }
}

static void fm_size_str(UINT32 s, char *out, int cap) {
    if (s >= 1024 * 1024) ksnprintf(out, (size_t)cap, "%u.%u MB", s >> 20, ((s & 0xFFFFF) * 10) >> 20);
    else if (s >= 1024)   ksnprintf(out, (size_t)cap, "%u KB", (s + 1023) >> 10);
    else                  ksnprintf(out, (size_t)cap, "%u bayt", s);
}

static void fm_paint(window_t *w, int cx, int cy, int cw, int ch) {
    (void)w;
    for (int i = 0; i < 5; i++) {
        gfx_rect(cx + fm_btn[i].x, cy + 4, fm_btn[i].w, 22, i == 3 ? 0x8A3A3A : 0x3E4B76);
        gfx_text(cx + fm_btn[i].x + (fm_btn[i].w - gfx_text_width(fm_btn[i].label)) / 2, cy + 7, fm_btn[i].label, 0xFFFFFF);
    }
    gfx_rect(cx + 6, cy + 32, cw - 12, 22, 0x14161F);
    gfx_rect_outline(cx + 6, cy + 32, cw - 12, 22, 0x454B63);
    gfx_text(cx + 12, cy + 35, fm_path, 0xE6E9F5);

    gfx_text(cx + 34, cy + 62, "Ad", COL_ACC);
    gfx_text(cx + cw - 130, cy + 62, "Boyut", COL_ACC);
    gfx_rect(cx + 6, cy + FM_LIST_Y - 4, cw - 12, 1, 0x454B63);

    for (int r = 0; r < FM_ROWS && fm_top + r < fm_n; r++) {
        int i = fm_top + r;
        int y = cy + FM_LIST_Y + r * FM_ROW_H;
        if (i == fm_sel) gfx_rect(cx + 4, y - 1, cw - 8, FM_ROW_H, 0x35508F);
        fm_ent_t *e = &fm_e[i];
        if (e->is_dir) { gfx_rect(cx + 10, y + 4, 16, 11, 0xE6B84A); gfx_rect(cx + 10, y + 2, 7, 3, 0xE6B84A); }
        else { gfx_rect(cx + 12, y + 2, 12, 15, 0xDADDE8); gfx_rect_outline(cx + 12, y + 2, 12, 15, 0x7A8098); }
        gfx_text(cx + 34, y, e->name, e->is_dir ? 0xFFE3A0 : COL_TEXT);
        if (!e->is_dir) { char s[24]; fm_size_str(e->size, s, sizeof s); gfx_text(cx + cw - 130, y, s, COL_DIM); }
        else gfx_text(cx + cw - 130, y, "<dizin>", COL_DIM);
    }
    if (fm_n == 0) gfx_text(cx + 34, cy + FM_LIST_Y, "(bos)", COL_DIM);

    gfx_rect(cx, cy + ch - 24, cw, 24, 0x252838);
    char b[160];
    if (fm_mode) {
        ksnprintf(b, sizeof b, "%s: %s_   (Enter: tamam, Esc: iptal)", fm_mode == 1 ? "Yeni klasor adi" : (fm_mode == 2 ? "Yeni dosya adi" : "Yeni ad"), fm_in);
        gfx_text(cx + 8, cy + ch - 20, b, 0xFFD27A);
    } else if (fm_msg[0] && timer_get_ticks() < fm_msg_until) {
        gfx_text(cx + 8, cy + ch - 20, fm_msg, 0xFFD27A);
    } else {
        UINT64 tot = 0, fr = 0;
        fs_space(fm_path, &tot, &fr);
        ksnprintf(b, sizeof b, "%d oge   |   %llu MB bos   |   Cift tikla: ac   Enter: ac   Backspace: yukari", fm_n, fr >> 20);
        gfx_text(cx + 8, cy + ch - 20, b, COL_DIM);
    }
}

static void fm_click(window_t *w, int lx, int ly, int buttons) {
    (void)w; (void)buttons;
    if (ly >= 4 && ly < 26) {
        for (int i = 0; i < 5; i++) if (lx >= fm_btn[i].x && lx < fm_btn[i].x + fm_btn[i].w) { fm_action(i); break; }
    } else if (ly >= FM_LIST_Y - 2) {
        int row = (ly - FM_LIST_Y) / FM_ROW_H + fm_top;
        if (row >= 0 && row < fm_n) {
            UINT64 now = timer_get_ticks();
            int dbl = row == fm_last_row && now - fm_last_click < 45;
            fm_sel = row; fm_confirm = -1;
            fm_last_row = row; fm_last_click = now;
            if (dbl) { fm_last_row = -1; fm_open_sel(); }
        }
    }
    wm_invalidate_window(g_fm);
}

static void fm_key(window_t *w, char c) {
    (void)w;
    if (fm_mode) {
        if (c == '\n') fm_prompt_done();
        else if (c == KEY_ESC) fm_mode = 0;
        else if (c == '\b') { if (fm_inl) fm_inl--; }
        else if (c >= 32 && c < 127 && fm_inl < 60) fm_in[fm_inl++] = c;
        wm_invalidate_window(g_fm);
        return;
    }
    switch (c) {
    case KEY_UP:   if (fm_sel > 0) fm_sel--; fm_confirm = -1; break;
    case KEY_DOWN: if (fm_sel + 1 < fm_n) fm_sel++; fm_confirm = -1; break;
    case KEY_PGUP: fm_sel -= FM_ROWS; if (fm_sel < 0) fm_sel = 0; break;
    case KEY_PGDN: fm_sel += FM_ROWS; if (fm_sel >= fm_n) fm_sel = fm_n - 1; break;
    case KEY_HOME: fm_sel = 0; break;
    case KEY_END:  fm_sel = fm_n - 1; break;
    case '\n':     fm_open_sel(); break;
    case '\b':     fm_up(); break;
    case KEY_DELETE: fm_delete_sel(); break;
    default: break;
    }
    fm_fit();
    wm_invalidate_window(g_fm);
}

static int fm_tick(window_t *w, UINT64 t) { (void)w; return fm_msg[0] && t >= fm_msg_until && t < fm_msg_until + 50; }
static void fm_close(window_t *w) { (void)w; g_fm = 0; }

void app_files_open(void) {
    if (g_fm) { wm_activate(g_fm); return; }
    g_fm = wm_create("Dosyalar", 170, 60, FM_W, FM_H);
    if (!g_fm) return;
    g_fm->paint = fm_paint; g_fm->on_click = fm_click; g_fm->on_key = fm_key; g_fm->on_tick = fm_tick;
    g_fm->tick_interval = 50; g_fm->on_close = fm_close;
    fm_mode = 0; fm_msg[0] = 0;
    fm_reload();
}

// ============================================================== HESAP MAKINESI

static window_t *g_calc;
static long long c_acc, c_cur;
static char c_op;
static int  c_fresh, c_err;
static const char *c_keys[4] = { "789/", "456*", "123-", "C0=+" };

static void calc_press(char k) {
    if (k >= '0' && k <= '9') {
        if (c_err) { c_acc = c_cur = 0; c_op = 0; c_err = 0; }
        if (c_fresh) { c_cur = 0; c_fresh = 0; }
        if (c_cur < 100000000000000LL) c_cur = c_cur * 10 + (k - '0');
    } else if (k == 'C' || k == 'c') {
        c_acc = c_cur = 0; c_op = 0; c_fresh = 0; c_err = 0;
    } else if (k == '+' || k == '-' || k == '*' || k == '/' || k == '=' || k == '\n') {
        if (c_err) return;
        if (c_op && !c_fresh) {
            long long r = c_cur;
            if (c_op == '+') r = c_acc + c_cur;
            else if (c_op == '-') r = c_acc - c_cur;
            else if (c_op == '*') r = c_acc * c_cur;
            else if (c_op == '/') { if (c_cur == 0) { c_err = 1; return; } r = c_acc / c_cur; }
            c_cur = r;
        }
        c_acc = c_cur;
        c_op = (k == '=' || k == '\n') ? 0 : k;
        c_fresh = 1;
    } else return;
    wm_invalidate_window(g_calc);
}

static void calc_paint(window_t *w, int cx, int cy, int cw, int ch) {
    (void)w; (void)cw; (void)ch;
    gfx_rect(cx + 8, cy + 8, 224, 56, 0x14161F);
    gfx_rect_outline(cx + 8, cy + 8, 224, 56, 0x454B63);
    char b[32];
    if (c_err) ksnprintf(b, sizeof b, "Hata");
    else       ksnprintf(b, sizeof b, "%lld", c_cur);
    int tw = (int)strlen(b) * 16;
    gfx_text_scaled(cx + 8 + 224 - 8 - tw, cy + 8 + 12, b, 0xE6E9F5, 2, 255);
    if (c_op) { char o[2] = { c_op, 0 }; gfx_text(cx + 14, cy + 12, o, COL_ACC); }
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            char k = c_keys[r][c];
            UINT32 col = (k >= '0' && k <= '9') ? 0x353A4D : (k == 'C' ? 0xA03A3A : (k == '=' ? 0x2E8B57 : 0x3E63B8));
            int bx = cx + 8 + c * 57, by = cy + 72 + r * 57;
            gfx_rect(bx, by, 52, 52, col);
            gfx_rect_outline(bx, by, 52, 52, gfx_mix(col, 0xFFFFFF, 70));
            char l[2] = { k, 0 };
            gfx_text_scaled(bx + 18, by + 10, l, 0xFFFFFF, 2, 255);
        }
}

static void calc_click(window_t *w, int lx, int ly, int buttons) {
    (void)w; (void)buttons;
    int c = (lx - 8) / 57, r = (ly - 72) / 57;
    if (lx < 8 || ly < 72 || c < 0 || c > 3 || r < 0 || r > 3) return;
    if ((lx - 8) % 57 >= 52 || (ly - 72) % 57 >= 52) return;
    calc_press(c_keys[r][c]);
}

static void calc_key(window_t *w, char c) { (void)w; if (c == '\b') { c_cur /= 10; wm_invalidate_window(g_calc); } else calc_press(c); }
static void calc_close(window_t *w) { (void)w; g_calc = 0; }

void app_calc_open(void) {
    if (g_calc) { wm_activate(g_calc); return; }
    c_acc = c_cur = 0; c_op = 0; c_fresh = 0; c_err = 0;
    g_calc = wm_create("Hesap Makinesi", 520, 120, 240, 304);
    if (!g_calc) return;
    g_calc->paint = calc_paint; g_calc->on_click = calc_click; g_calc->on_key = calc_key; g_calc->on_close = calc_close;
}

// ===================================================================== HAKKINDA

static window_t *g_about;
static char about_cpu[64];

static void about_row(int x, int y, const char *label, const char *value) {
    gfx_text(x, y, label, COL_DIM);
    gfx_text(x + 128, y, value, COL_TEXT);
}

static void about_paint(window_t *w, int cx, int cy, int cw, int ch) {
    (void)w; (void)ch;
    gfx_vgradient(cx, cy, cw, 86, 0x2F4F94, 0x203768);
    gfx_text_scaled(cx + 20, cy + 16, "DEOS", 0xFFFFFF, 4, 255);
    gfx_text(cx + 20 + 4 * 32 + 20, cy + 24, "Surum " DEOS_VERSION, 0xFFFFFF);
    gfx_text(cx + 20 + 4 * 32 + 20, cy + 44, "x86_64 isletim sistemi", 0xBFD0FF);

    char b[96];
    int x = cx + 20, y = cy + 106;
    about_row(x, y, "Cekirdek", "DEOS " DEOS_VERSION " (x86_64, preemptive)"); y += 24;
    about_row(x, y, "Onyukleme", "UEFI"); y += 24;
    about_row(x, y, "Islemci", about_cpu); y += 24;

    pmm_stats_t ps; pmm_get_stats(&ps);
    ksnprintf(b, sizeof b, "%llu MB (%llu MB bos)", ps.usable_pages * 4 / 1024, ps.free_pages * 4 / 1024);
    about_row(x, y, "Bellek", b); y += 24;

    ksnprintf(b, sizeof b, "%d x %d", gfx_width(), gfx_height());
    about_row(x, y, "Ekran", b); y += 24;

    blockdev_t *d = blk_get(0);
    if (d) ksnprintf(b, sizeof b, "%s, %llu MB", d->model[0] ? d->model : "SATA disk", d->sectors / 2048);
    else   ksnprintf(b, sizeof b, "yok");
    about_row(x, y, "Disk", b); y += 24;

    UINT64 s = timer_get_ticks() / 100;
    ksnprintf(b, sizeof b, "%llu sa %llu dk %llu sn", s / 3600, (s / 60) % 60, s % 60);
    about_row(x, y, "Calisma suresi", b); y += 24;

    thread_info_t ti; int n = 0, users = 0;
    for (int i = 0; thread_get_info(i, &ti); i++) { if (!ti.is_idle) n++; if (ti.is_user) users++; }
    ksnprintf(b, sizeof b, "%d thread, %d kullanici sureci", n, users);
    about_row(x, y, "Surecler", b); y += 30;

    gfx_rect(x, y, cw - 40, 1, 0x454B63);
    gfx_text(x, y + 12, DEOS_COPYRIGHT, COL_DIM);
}

static int about_tick(window_t *w, UINT64 t) { (void)w; (void)t; return 1; }
static void about_close(window_t *w) { (void)w; g_about = 0; }

void app_about_open(void) {
    if (g_about) { wm_activate(g_about); return; }
    cpu_brand(about_cpu);
    about_cpu[sizeof about_cpu - 1] = 0;
    g_about = wm_create("Hakkinda", 260, 120, 520, 350);
    if (!g_about) return;
    g_about->paint = about_paint; g_about->on_close = about_close;
    g_about->on_tick = about_tick; g_about->tick_interval = 100;
}

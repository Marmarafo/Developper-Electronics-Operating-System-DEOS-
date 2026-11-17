// DEOS -- TCP istemcisi (RFC 793 / 5681'in sade alt kumesi): aktif acilis (connect), guvenilir sirali iletim,
// akis kontrolu, yeniden iletim (geri-N + ustel geri cekilme), FIN ile kapanis, RST isleme.
//
// Kapsam ve sinirlar:
//  - Yalnizca istemci: listen/accept YOK. En fazla 8 es zamanli baglanti; her yonde 16 KB halka tampon.
//  - Pencere olcekleme, SACK, zaman damgasi yok. Gonderim penceresi en fazla 4 MSS (MSS 1460).
//  - Sirasiz gelen veri ATILIR (tekrar iletim beklenir); cift ACK ile hizli yeniden iletim yok.
//  - Sabit RTO tabani 300 ms (RTT olcumu yok), her zaman asiminda ikiye katlanir (en fazla 4 sn), 8 denemeden sonra RST.
//  - TIME_WAIT 2 sn (RFC'nin 2*MSL'sinden cok kisa; gecici portlar rastgele secilir).
//  - Tum durum degisiklikleri kisa kritik bolgelerde (kesmeler kapali) yapilir; ip_output bu sirada ARP BEKLEMEZ
//    (bkz. net.c): ilk SYN'den once tcp_connect, sonraki sekmeyi net_resolve_nexthop ile onceden cozer.
#include "net.h"
#include "config.h"
#include "net_priv.h"
#include "types.h"
#include "heap.h"
#include "string.h"
#include "thread.h"
#include "timer.h"
#include "rand.h"
#include "kprintf.h"

#define TCP_MAX   8
#define BUFSZ     16384u                  // 2'nin kuvveti (halka maskesi)
#define MSS_MAX   1460
#define RTO_BASE  (TIMER_HZ * 3 / 10)     // 300 ms
#define RTO_MAX   (TIMER_HZ * 4)
#define MAX_RETRY 8
#define TW_TICKS  (TIMER_HZ * 2)

#define F_FIN 0x01
#define F_SYN 0x02
#define F_RST 0x04
#define F_PSH 0x08
#define F_ACK 0x10

enum { TS_FREE = 0, TS_SYN_SENT, TS_ESTABLISHED, TS_FIN_WAIT1, TS_FIN_WAIT2, TS_CLOSE_WAIT, TS_CLOSING, TS_LAST_ACK, TS_TIME_WAIT, TS_CLOSED };

#define SEQ_LT(a, b)  ((int)((a) - (b)) < 0)
#define SEQ_LEQ(a, b) ((int)((a) - (b)) <= 0)

typedef struct {
    int used, state, app_closed, error;
    UINT32 rip; UINT16 rport, lport;
    UINT32 iss, snd_una, snd_nxt, snd_wnd, irs, rcv_nxt;
    UINT32 mss;
    UINT8 *snd, *rcv;
    UINT32 snd_head, snd_len, rcv_head, rcv_len;       // halka: baslangic indeksi + doluluk
    int fin_pending, fin_sent, peer_fin;
    UINT32 fin_seq;
    UINT64 rto_deadline; UINT32 rto_ticks; int retries;
    UINT64 tw_until;
} tcb_t;

static tcb_t g_t[TCP_MAX];

const char *tcp_state_name(int st) {
    static const char *n[] = { "FREE", "SYN_SENT", "ESTABLISHED", "FIN_WAIT1", "FIN_WAIT2", "CLOSE_WAIT", "CLOSING", "LAST_ACK", "TIME_WAIT", "CLOSED" };
    return (st >= 0 && st <= TS_CLOSED) ? n[st] : "?";
}

static UINT32 ms_to_ticks(UINT32 ms) { UINT64 t = ((UINT64)ms * TIMER_HZ + 999) / 1000; return t ? (UINT32)t : 1; }
static UINT32 min3(UINT32 a, UINT32 b, UINT32 c) { UINT32 m = a < b ? a : b; return m < c ? m : c; }

// ---- halka tampon ----
static void ring_put(UINT8 *ring, UINT32 head, UINT32 len, const UINT8 *src, UINT32 n) {
    UINT32 pos = (head + len) & (BUFSZ - 1);
    UINT32 first = BUFSZ - pos; if (first > n) first = n;
    memcpy(ring + pos, src, first);
    if (n > first) memcpy(ring, src + first, n - first);
}
static void ring_get(const UINT8 *ring, UINT32 head, UINT32 off, UINT8 *dst, UINT32 n) {
    UINT32 pos = (head + off) & (BUFSZ - 1);
    UINT32 first = BUFSZ - pos; if (first > n) first = n;
    memcpy(dst, ring + pos, first);
    if (n > first) memcpy(dst + first, ring, n - first);
}

// ---- segment gonderme (kritik bolge icinden cagrilir; ip_output ARP beklemez) ----
static void send_raw(UINT32 rip, UINT16 lport, UINT16 rport, UINT32 seq, UINT32 ack, UINT8 flags, UINT32 wnd,
                     const UINT8 *data, UINT32 len, int with_mss) {
    UINT8 seg[24 + MSS_MAX];
    UINT32 hl = 20;
    put16(seg, lport); put16(seg + 2, rport); put32(seg + 4, seq); put32(seg + 8, ack);
    if (with_mss) { seg[20] = 2; seg[21] = 4; put16(seg + 22, MSS_MAX); hl = 24; }
    seg[12] = (UINT8)((hl / 4) << 4); seg[13] = flags;
    put16(seg + 14, (UINT16)(wnd > 65535 ? 65535 : wnd));
    seg[16] = seg[17] = 0; seg[18] = seg[19] = 0;
    if (len) memcpy(seg + hl, data, len);
    UINT32 tl = hl + len;
    put16(seg + 16, l4_csum(net_ip(), rip, IPPROTO_TCP, seg, tl));
    ip_output(net_ip(), rip, IPPROTO_TCP, seg, tl);
}

static UINT32 rcv_free(const tcb_t *c) { return c->rcv ? BUFSZ - c->rcv_len : 0; }

static void send_seg(tcb_t *c, UINT32 seq, UINT8 flags, const UINT8 *data, UINT32 len) {
    send_raw(c->rip, c->lport, c->rport, seq, (flags & F_ACK) ? c->rcv_nxt : 0, flags, rcv_free(c), data, len, (flags & F_SYN) != 0);
}

static void arm(tcb_t *c) { c->rto_deadline = timer_get_ticks() + c->rto_ticks; }

static void release(tcb_t *c) {
    kfree(c->snd); kfree(c->rcv);
    memset(c, 0, sizeof *c);
}

// Gonderilebilecek veriyi (pencere izin verdigince) ve gerekirse FIN'i yollar.
static void tcp_output(tcb_t *c) {
    int st = c->state;
    if (st != TS_ESTABLISHED && st != TS_CLOSE_WAIT && st != TS_FIN_WAIT1 && st != TS_LAST_ACK) return;
    UINT32 sent = c->snd_nxt - c->snd_una;
    UINT32 data_sent = c->fin_sent ? (sent ? sent - 1 : 0) : sent;
    UINT8 tmp[MSS_MAX];
    while (data_sent < c->snd_len) {
        UINT32 avail = c->snd_len - data_sent, cap = 4 * c->mss;
        UINT32 wnd = c->snd_wnd < cap ? c->snd_wnd : cap;
        UINT32 n;
        if (wnd > data_sent) n = min3(avail, wnd - data_sent, c->mss);
        else if (c->snd_wnd == 0 && data_sent == 0) n = 1;           // sifir pencere: 1 baytlik sorgu
        else break;
        ring_get(c->snd, c->snd_head, data_sent, tmp, n);
        send_seg(c, c->snd_nxt, F_ACK | F_PSH, tmp, n);
        c->snd_nxt += n; data_sent += n;
        if (!c->rto_deadline) arm(c);
    }
    if (c->fin_pending && !c->fin_sent && data_sent >= c->snd_len) {
        send_seg(c, c->snd_nxt, F_FIN | F_ACK, 0, 0);
        c->fin_seq = c->snd_nxt; c->snd_nxt++; c->fin_sent = 1;
        if (st == TS_ESTABLISHED) c->state = TS_FIN_WAIT1; else if (st == TS_CLOSE_WAIT) c->state = TS_LAST_ACK;
        if (!c->rto_deadline) arm(c);
    }
}

static void send_ack(tcb_t *c) { send_seg(c, c->snd_nxt, F_ACK, 0, 0); }

static void abort_conn(tcb_t *c, int err, int send_rst) {
    if (send_rst && c->state != TS_SYN_SENT) send_raw(c->rip, c->lport, c->rport, c->snd_nxt, c->rcv_nxt, F_RST | F_ACK, 0, 0, 0, 0);
    c->error = err; c->state = TS_CLOSED; c->rto_deadline = 0;
}

// Bilinmeyen baglantiya gelen segmente RST ile yanit ver (RFC 793 s.3.4).
static void reply_rst(UINT32 src, UINT32 dst, const UINT8 *seg, UINT32 len, UINT32 hl) {
    UINT8 fl = seg[13];
    if (fl & F_RST) return;
    UINT32 seq = be32(seg + 4), ack = be32(seg + 8), plen = len - hl;
    (void)dst;
    UINT16 sp = be16(seg), dp = be16(seg + 2);
    if (fl & F_ACK) send_raw(src, dp, sp, ack, 0, F_RST, 0, 0, 0, 0);
    else send_raw(src, dp, sp, 0, seq + plen + ((fl & F_SYN) ? 1 : 0) + ((fl & F_FIN) ? 1 : 0), F_RST | F_ACK, 0, 0, 0, 0);
}

void tcp_input(UINT32 src, UINT32 dst, const UINT8 *seg, UINT32 len) {
    if (len < 20) return;
    UINT32 hl = (UINT32)(seg[12] >> 4) * 4;
    if (hl < 20 || hl > len) return;
    if (l4_csum(src, dst, IPPROTO_TCP, seg, len) != 0) return;       // saglama hatali
    UINT16 sp = be16(seg), dp = be16(seg + 2);
    UINT32 seq = be32(seg + 4), ack = be32(seg + 8), win = be16(seg + 14);
    UINT8 fl = seg[13];
    const UINT8 *pl = seg + hl; UINT32 plen = len - hl;

    NET_CRIT_BEGIN();
    tcb_t *c = 0;
    for (int i = 0; i < TCP_MAX; i++)
        if (g_t[i].used && g_t[i].rip == src && g_t[i].rport == sp && g_t[i].lport == dp) { c = &g_t[i]; break; }
    if (!c || c->state == TS_CLOSED) { reply_rst(src, dst, seg, len, hl); NET_CRIT_END(); return; }

    if (c->state == TS_SYN_SENT) {
        if (fl & F_ACK) {
            if (ack != c->iss + 1) { if (!(fl & F_RST)) send_raw(src, dp, sp, ack, 0, F_RST, 0, 0, 0, 0); NET_CRIT_END(); return; }
            if (fl & F_RST) { abort_conn(c, TCP_ERR_RESET, 0); NET_CRIT_END(); return; }
        } else if (fl & F_RST) { NET_CRIT_END(); return; }
        if ((fl & F_SYN) && (fl & F_ACK)) {
            UINT32 mss = 536;                                         // RFC 1122 varsayilani
            for (UINT32 i = 20; i + 1 < hl; ) {                        // secenekler
                UINT8 k = seg[i];
                if (k == 0) break;
                if (k == 1) { i++; continue; }
                UINT8 ol = seg[i + 1];
                if (ol < 2 || i + ol > hl) break;
                if (k == 2 && ol == 4) mss = be16(seg + i + 2);
                i += ol;
            }
            if (mss < 64) mss = 536;
            c->mss = mss < MSS_MAX ? mss : MSS_MAX;
            c->irs = seq; c->rcv_nxt = seq + 1;
            c->snd_una = c->iss + 1; c->snd_nxt = c->iss + 1; c->snd_wnd = win;
            c->state = TS_ESTABLISHED; c->rto_deadline = 0; c->retries = 0; c->rto_ticks = RTO_BASE;
            send_ack(c);
        }
        NET_CRIT_END();
        return;
    }

    // ---- senkronize durumlar ----
    if (fl & F_RST) {
        if (seq == c->rcv_nxt || (SEQ_LEQ(c->rcv_nxt, seq) && SEQ_LT(seq, c->rcv_nxt + BUFSZ))) abort_conn(c, TCP_ERR_RESET, 0);
        NET_CRIT_END();
        return;
    }
    if (fl & F_SYN) { send_ack(c); NET_CRIT_END(); return; }          // senkronize iken SYN: yoksay, ACK ver
    if (!(fl & F_ACK)) { NET_CRIT_END(); return; }

    int need_ack = 0, fin_acked = 0;
    if (SEQ_LT(c->snd_nxt, ack)) { send_ack(c); NET_CRIT_END(); return; }       // henuz gonderilmemis veriyi onayliyor
    if (SEQ_LT(c->snd_una, ack)) {
        UINT32 acked = ack - c->snd_una;
        if (c->fin_sent && ack == c->snd_nxt) { fin_acked = 1; acked--; }
        if (acked > c->snd_len) acked = c->snd_len;
        c->snd_head = (c->snd_head + acked) & (BUFSZ - 1);
        c->snd_len -= acked;
        c->snd_una = ack; c->retries = 0; c->rto_ticks = RTO_BASE;
        c->rto_deadline = (c->snd_una == c->snd_nxt) ? 0 : timer_get_ticks() + c->rto_ticks;
        c->snd_wnd = win;
    } else if (ack == c->snd_una) {
        c->snd_wnd = win;                                                // pencere guncellemesi
    }
    if (c->fin_sent && c->snd_una == c->snd_nxt) fin_acked = 1;

    // durum gecisleri (FIN onayi)
    if (fin_acked) {
        if (c->state == TS_FIN_WAIT1) c->state = c->peer_fin ? TS_TIME_WAIT : TS_FIN_WAIT2;
        else if (c->state == TS_CLOSING) c->state = TS_TIME_WAIT;
        else if (c->state == TS_LAST_ACK) { c->state = TS_CLOSED; c->rto_deadline = 0; }
        if (c->state == TS_TIME_WAIT) { c->tw_until = timer_get_ticks() + TW_TICKS; c->rto_deadline = 0; }
    }

    // veri (yalniz ESTABLISHED / FIN_WAIT1 / FIN_WAIT2 icinde kabul edilir)
    int in_state = c->state == TS_ESTABLISHED || c->state == TS_FIN_WAIT1 || c->state == TS_FIN_WAIT2;
    UINT32 accepted = 0;
    if (plen && in_state) {
        UINT32 skip = 0;
        if (SEQ_LT(seq, c->rcv_nxt)) { skip = c->rcv_nxt - seq; if (skip >= plen) skip = plen; }       // eski/ust uste binen kisim
        if (seq + skip == c->rcv_nxt && skip < plen) {
            UINT32 n = plen - skip, space = rcv_free(c);
            if (n > space) n = space;
            if (n) { ring_put(c->rcv, c->rcv_head, c->rcv_len, pl + skip, n); c->rcv_len += n; c->rcv_nxt += n; accepted = n; }
        }
        need_ack = 1;                                                    // kabul, tekrar ya da sirasiz: hepsi icin ACK
    }
    // FIN: yalnizca tum veri sirayla kabul edildiyse
    if ((fl & F_FIN) && in_state && !c->peer_fin && seq + plen == c->rcv_nxt) {
        c->rcv_nxt++; c->peer_fin = 1; need_ack = 1;
        if (c->state == TS_ESTABLISHED) c->state = TS_CLOSE_WAIT;
        else if (c->state == TS_FIN_WAIT1) c->state = TS_CLOSING;
        else if (c->state == TS_FIN_WAIT2) { c->state = TS_TIME_WAIT; c->tw_until = timer_get_ticks() + TW_TICKS; }
    } else if ((fl & F_FIN) && c->peer_fin) {
        need_ack = 1;                                                    // FIN tekrari
    }
    (void)accepted;
    if (need_ack) send_ack(c);
    tcp_output(c);                                                       // pencere acildiysa daha fazla veri gonder
    NET_CRIT_END();
}

void tcp_tick(void) {
    NET_CRIT_BEGIN();
    UINT64 now = timer_get_ticks();
    for (int i = 0; i < TCP_MAX; i++) {
        tcb_t *c = &g_t[i];
        if (!c->used) continue;
        if (c->state == TS_TIME_WAIT && now >= c->tw_until) c->state = TS_CLOSED;
        if (c->app_closed && c->state == TS_CLOSED) { release(c); continue; }
        if (!c->rto_deadline || now < c->rto_deadline) continue;
        if (c->state == TS_SYN_SENT) {
            if (++c->retries > 5) { abort_conn(c, TCP_ERR_TIMEOUT, 0); continue; }
            c->rto_ticks = c->rto_ticks * 2 > RTO_MAX ? RTO_MAX : c->rto_ticks * 2;
            send_seg(c, c->iss, F_SYN, 0, 0);
            arm(c);
        } else if (c->snd_una != c->snd_nxt) {
            if (++c->retries > MAX_RETRY) { abort_conn(c, TCP_ERR_TIMEOUT, 1); continue; }
            c->rto_ticks = c->rto_ticks * 2 > RTO_MAX ? RTO_MAX : c->rto_ticks * 2;
            c->snd_nxt = c->snd_una; c->fin_sent = 0;                    // geri-N: ilk onaylanmamis bayttan yeniden
            tcp_output(c);
            if (c->snd_una != c->snd_nxt) arm(c); else c->rto_deadline = 0;
        } else c->rto_deadline = 0;
    }
    NET_CRIT_END();
}

// ------------------------------------------------------------------ API
static tcb_t *get(int h) { return (h >= 0 && h < TCP_MAX && g_t[h].used && !g_t[h].app_closed) ? &g_t[h] : 0; }

int tcp_connect(UINT32 ip, UINT16 port, UINT32 timeout_ms) {
    if (!net_present() || !port || ip == 0 || net_is_broadcast(ip)) return TCP_ERR_ARG;
    UINT8 *snd = kmalloc(BUFSZ), *rcv = kmalloc(BUFSZ);
    if (!snd || !rcv) { kfree(snd); kfree(rcv); return TCP_ERR_NOMEM; }
    if (net_resolve_nexthop(ip, 1500) != 0) { kfree(snd); kfree(rcv); return TCP_ERR_TIMEOUT; }   // ARP, SYN'den ONCE

    NET_CRIT_BEGIN();
    int h = -1;
    for (int i = 0; i < TCP_MAX; i++) if (!g_t[i].used) { h = i; break; }
    if (h < 0) { NET_CRIT_END(); kfree(snd); kfree(rcv); return TCP_ERR_NOMEM; }
    UINT16 lport = 0;
    for (int tries = 0; tries < 64 && !lport; tries++) {
        UINT16 cand = (UINT16)(49152 + rand64() % 16384);
        int clash = 0;
        for (int i = 0; i < TCP_MAX; i++) if (g_t[i].used && g_t[i].lport == cand) clash = 1;
        if (!clash) lport = cand;
    }
    tcb_t *c = &g_t[h];
    memset(c, 0, sizeof *c);
    c->used = 1; c->state = TS_SYN_SENT; c->rip = ip; c->rport = port; c->lport = lport;
    c->snd = snd; c->rcv = rcv; c->mss = 536;
    c->iss = (UINT32)rand64(); c->snd_una = c->iss; c->snd_nxt = c->iss + 1;
    c->rto_ticks = RTO_BASE;
    send_seg(c, c->iss, F_SYN, 0, 0);
    arm(c);
    NET_CRIT_END();

    UINT64 start = timer_get_ticks(), tmo = ms_to_ticks(timeout_ms);
    for (;;) {
        NET_CRIT_BEGIN();
        int st = c->state, err = c->error;
        NET_CRIT_END();
        if (st == TS_ESTABLISHED) return h;
        if (st == TS_CLOSED) { NET_CRIT_BEGIN(); release(c); NET_CRIT_END(); return err ? err : TCP_ERR_CLOSED; }
        if (timer_get_ticks() - start >= tmo) {
            NET_CRIT_BEGIN(); release(c); NET_CRIT_END();
            return TCP_ERR_TIMEOUT;
        }
        net_poll_once();
        thread_sleep_ms(1);
    }
}

static int fail_code(const tcb_t *c) { return c->error ? c->error : TCP_ERR_CLOSED; }

int tcp_write(int h, const void *data, UINT32 len, UINT32 timeout_ms) {
    const UINT8 *p = data;
    UINT32 done = 0;
    UINT64 start = timer_get_ticks(), tmo = ms_to_ticks(timeout_ms);
    while (done < len) {
        NET_CRIT_BEGIN();
        tcb_t *c = get(h);
        if (!c) { NET_CRIT_END(); return done ? (int)done : TCP_ERR_ARG; }
        if (c->state != TS_ESTABLISHED && c->state != TS_CLOSE_WAIT) {
            int e = fail_code(c); NET_CRIT_END();
            return done ? (int)done : e;
        }
        if (c->fin_pending) { NET_CRIT_END(); return done ? (int)done : TCP_ERR_CLOSED; }
        UINT32 n = BUFSZ - c->snd_len;
        if (n > len - done) n = len - done;
        if (n > 4096) n = 4096;                                          // kritik bolgeyi kisa tut
        if (n) {
            ring_put(c->snd, c->snd_head, c->snd_len, p + done, n);
            c->snd_len += n; done += n;
            tcp_output(c);
        }
        NET_CRIT_END();
        if (!n) {
            if (timer_get_ticks() - start >= tmo) return done ? (int)done : TCP_ERR_TIMEOUT;
            net_poll_once();
            thread_sleep_ms(1);
        }
    }
    return (int)done;
}

int tcp_read(int h, void *buf, UINT32 max, UINT32 timeout_ms) {
    UINT64 start = timer_get_ticks(), tmo = ms_to_ticks(timeout_ms);
    for (;;) {
        NET_CRIT_BEGIN();
        tcb_t *c = get(h);
        if (!c) { NET_CRIT_END(); return TCP_ERR_ARG; }
        if (c->rcv_len > 0) {
            UINT32 old_free = rcv_free(c);
            UINT32 n = c->rcv_len < max ? c->rcv_len : max;
            ring_get(c->rcv, c->rcv_head, 0, buf, n);
            c->rcv_head = (c->rcv_head + n) & (BUFSZ - 1);
            c->rcv_len -= n;
            if (old_free < c->mss && rcv_free(c) >= c->mss && c->state != TS_CLOSED) send_ack(c);      // pencere guncelleme
            NET_CRIT_END();
            return (int)n;
        }
        if (c->peer_fin) { NET_CRIT_END(); return 0; }
        if (c->state == TS_CLOSED || c->state == TS_TIME_WAIT) { int e = fail_code(c); NET_CRIT_END(); return e; }
        NET_CRIT_END();
        if (timer_get_ticks() - start >= tmo) return TCP_ERR_TIMEOUT;
        net_poll_once();
        thread_sleep_ms(1);
    }
}

int tcp_close(int h) {
    NET_CRIT_BEGIN();
    tcb_t *c = get(h);
    if (!c) { NET_CRIT_END(); return TCP_ERR_ARG; }
    c->app_closed = 1;                                                   // artik uygulama erisemez; tcp_tick serbest birakir
    if (c->state == TS_SYN_SENT) { c->state = TS_CLOSED; c->rto_deadline = 0; }
    else if (c->state == TS_ESTABLISHED || c->state == TS_CLOSE_WAIT) { c->fin_pending = 1; tcp_output(c); }
    NET_CRIT_END();
    return 0;
}

int tcp_state(int h) { return (h >= 0 && h < TCP_MAX && g_t[h].used) ? g_t[h].state : -1; }

int tcp_conn_count(void) {
    int n = 0;
    NET_CRIT_BEGIN();
    for (int i = 0; i < TCP_MAX; i++) if (g_t[i].used) n++;
    NET_CRIT_END();
    return n;
}

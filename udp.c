// DEOS -- UDP soketleri: soket tablosu (16), soket basina 8 datagramlik kuyruk, port/es filtresi.
// Giris dogrulamasi: baslik uzunlugu, UDP uzunlugu IP yukunu asamaz, saglama (sifir degilse) dogrulanir.
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

#define UDP_SOCKS 16
#define UDPQ 8

typedef struct { UINT32 ip; UINT16 port; UINT16 len; UINT8 data[UDP_MAX_PAYLOAD]; } upkt_t;
typedef struct {
    int used, connected;
    UINT16 port, peer_port;
    UINT32 peer_ip;
    upkt_t *q;
    int head, count;
    UINT32 dropped;
} usock_t;

static usock_t g_s[UDP_SOCKS];

static UINT32 ms_to_ticks(UINT32 ms) { UINT64 t = ((UINT64)ms * TIMER_HZ + 999) / 1000; return t ? (UINT32)t : 1; }

static int port_in_use(UINT16 port) {
    for (int i = 0; i < UDP_SOCKS; i++) if (g_s[i].used && g_s[i].port == port) return 1;
    return 0;
}

int udp_open(UINT16 port) {
    upkt_t *q = kmalloc(sizeof(upkt_t) * UDPQ);
    if (!q) return -1;
    NET_CRIT_BEGIN();
    int slot = -1;
    if (port != 0 && port_in_use(port)) { NET_CRIT_END(); kfree(q); return -1; }
    for (int i = 0; i < UDP_SOCKS; i++) if (!g_s[i].used) { slot = i; break; }
    if (slot < 0) { NET_CRIT_END(); kfree(q); return -1; }
    if (port == 0) {                                          // gecici port: 49152..65535
        for (int tries = 0; tries < 64; tries++) {
            UINT16 c = (UINT16)(49152 + rand64() % 16384);
            if (!port_in_use(c)) { port = c; break; }
        }
        if (!port) { NET_CRIT_END(); kfree(q); return -1; }
    }
    usock_t *s = &g_s[slot];
    memset(s, 0, sizeof *s);
    s->used = 1; s->port = port; s->q = q;
    NET_CRIT_END();
    return slot;
}

void udp_close(int sd) {
    if (sd < 0 || sd >= UDP_SOCKS) return;
    NET_CRIT_BEGIN();
    upkt_t *q = g_s[sd].q;
    int was = g_s[sd].used;
    memset(&g_s[sd], 0, sizeof g_s[sd]);
    NET_CRIT_END();
    if (was) kfree(q);
}

int udp_connect(int sd, UINT32 ip, UINT16 port) {
    if (sd < 0 || sd >= UDP_SOCKS) return -1;
    NET_CRIT_BEGIN();
    int ok = g_s[sd].used;
    if (ok) { g_s[sd].connected = 1; g_s[sd].peer_ip = ip; g_s[sd].peer_port = port; }
    NET_CRIT_END();
    return ok ? 0 : -1;
}

int udp_local_port(int sd) { return (sd >= 0 && sd < UDP_SOCKS && g_s[sd].used) ? g_s[sd].port : -1; }
UINT32 udp_dropped(int sd) { return (sd >= 0 && sd < UDP_SOCKS && g_s[sd].used) ? g_s[sd].dropped : 0; }

int udp_sendto_from(int sd, UINT32 src, UINT32 ip, UINT16 port, const void *data, UINT32 len) {
    if (sd < 0 || sd >= UDP_SOCKS || !g_s[sd].used || len > UDP_MAX_PAYLOAD || (!data && len)) return -1;
    UINT8 seg[8 + UDP_MAX_PAYLOAD];
    put16(seg, g_s[sd].port); put16(seg + 2, port); put16(seg + 4, (UINT16)(8 + len)); seg[6] = seg[7] = 0;
    if (len) memcpy(seg + 8, data, len);
    UINT16 c = l4_csum(src, ip, IPPROTO_UDP, seg, 8 + len);
    put16(seg + 6, c ? c : 0xFFFF);                           // 0 = "saglama yok" anlamina gelir
    return ip_output(src, ip, IPPROTO_UDP, seg, 8 + len);
}

int udp_sendto(int sd, UINT32 ip, UINT16 port, const void *data, UINT32 len) {
    return udp_sendto_from(sd, net_ip(), ip, port, data, len);
}

int udp_send(int sd, const void *data, UINT32 len) {
    if (sd < 0 || sd >= UDP_SOCKS || !g_s[sd].used || !g_s[sd].connected) return -1;
    return udp_sendto(sd, g_s[sd].peer_ip, g_s[sd].peer_port, data, len);
}

// IP yuku dogrulanmis olarak gelir (net.c). Hedef portu dinleyen ilk sokete kuyruga eklenir.
void udp_input(UINT32 src, UINT32 dst, const UINT8 *seg, UINT32 len) {
    if (len < 8) return;
    UINT16 sp = be16(seg), dp = be16(seg + 2), ulen = be16(seg + 4);
    if (ulen < 8 || ulen > len) return;                       // UDP uzunlugu IP yukunu asamaz
    if (be16(seg + 6) != 0 && l4_csum(src, dst, IPPROTO_UDP, seg, ulen) != 0) return;   // saglama hatali
    UINT32 plen = ulen - 8;
    if (plen > UDP_MAX_PAYLOAD) return;
    NET_CRIT_BEGIN();
    for (int i = 0; i < UDP_SOCKS; i++) {
        usock_t *s = &g_s[i];
        if (!s->used || s->port != dp) continue;
        if (s->connected && (s->peer_ip != src || s->peer_port != sp)) continue;
        if (s->count >= UDPQ) { s->dropped++; break; }
        upkt_t *p = &s->q[(s->head + s->count) % UDPQ];
        p->ip = src; p->port = sp; p->len = (UINT16)plen;
        memcpy(p->data, seg + 8, plen);
        s->count++;
        break;
    }
    NET_CRIT_END();
}

int udp_recvfrom(int sd, void *buf, UINT32 max, UINT32 *from_ip, UINT16 *from_port, UINT32 timeout_ms) {
    if (sd < 0 || sd >= UDP_SOCKS) return -2;
    UINT64 start = timer_get_ticks(), tmo = ms_to_ticks(timeout_ms);
    for (;;) {
        NET_CRIT_BEGIN();
        usock_t *s = &g_s[sd];
        if (!s->used) { NET_CRIT_END(); return -2; }
        if (s->count > 0) {
            upkt_t *p = &s->q[s->head];
            UINT32 n = p->len < max ? p->len : max;
            if (buf && n) memcpy(buf, p->data, n);
            if (from_ip) *from_ip = p->ip;
            if (from_port) *from_port = p->port;
            s->head = (s->head + 1) % UDPQ; s->count--;
            NET_CRIT_END();
            return (int)n;
        }
        NET_CRIT_END();
        if (timer_get_ticks() - start >= tmo) return -1;
        net_poll_once();
        thread_sleep_ms(1);
    }
}

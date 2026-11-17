// DEOS -- DHCP istemcisi (RFC 2131): Discover -> Offer -> Request -> Ack; kira yenileme (T1 unicast, T2 yayin),
// kira dolunca yapilandirmayi birakip yeniden edinme. Yayin bayragi acik (sunucu cevabi yayin yapabilir).
// Edinme sirasinda gonderen adresi 0.0.0.0'dir; yapilandirma yalnizca Ack ile degisir.
#include "net.h"
#include "config.h"
#include "net_priv.h"
#include "types.h"
#include "string.h"
#include "thread.h"
#include "timer.h"
#include "rand.h"
#include "kprintf.h"

#define DHCP_DISCOVER_T 1
#define DHCP_OFFER_T    2
#define DHCP_REQUEST_T  3
#define DHCP_ACK_T      5
#define DHCP_NAK_T      6
#define BOOTP_LEN 300
#define COOKIE 0x63825363u

static volatile int g_busy;                 // edinme/yenileme calisiyor
static UINT64 g_bound_tick;                 // Ack'in alindigi an
static UINT32 g_t1_s, g_t2_s, g_lease_s;    // saniye cinsinden (0xFFFFFFFF = sonsuz)
static volatile int g_renew_due;            // dhcp_tick -> is parcacigi tetikleyici

static UINT32 ms_to_ticks(UINT32 ms) { UINT64 t = ((UINT64)ms * TIMER_HZ + 999) / 1000; return t ? (UINT32)t : 1; }

const char *dhcp_state_name(int st) {
    switch (st) {
    case DHCP_IDLE: return "yok (statik)";
    case DHCP_DISCOVER: return "kesif (discover)";
    case DHCP_REQUEST: return "istek (request)";
    case DHCP_BOUND: return "baglandi (bound)";
    case DHCP_RENEWING: return "yenileniyor";
    case DHCP_FAILED: return "basarisiz";
    default: return "?";
    }
}

int dhcp_state(void) { net_status_t s; net_get_status(&s); return s.dhcp_state; }

// ---- paket olusturma / ayristirma ----
static int build(UINT8 *p, int type, UINT32 xid, UINT32 ciaddr, UINT32 req_ip, UINT32 server, int bcast) {
    memset(p, 0, BOOTP_LEN);
    p[0] = 1; p[1] = 1; p[2] = 6;                             // BOOTREQUEST, ethernet, hlen 6
    put32(p + 4, xid);
    put16(p + 10, bcast ? 0x8000 : 0);
    memcpy(p + 12, &ciaddr, 4);
    memcpy(p + 28, net_mac(), 6);
    put32(p + 236, COOKIE);
    int o = 240;
    p[o++] = 53; p[o++] = 1; p[o++] = (UINT8)type;
    if (req_ip) { p[o++] = 50; p[o++] = 4; memcpy(p + o, &req_ip, 4); o += 4; }
    if (server) { p[o++] = 54; p[o++] = 4; memcpy(p + o, &server, 4); o += 4; }
    p[o++] = 61; p[o++] = 7; p[o++] = 1; memcpy(p + o, net_mac(), 6); o += 6;       // istemci kimligi
    p[o++] = 55; p[o++] = 5; p[o++] = 1; p[o++] = 3; p[o++] = 6; p[o++] = 15; p[o++] = 51;
    p[o++] = 255;
    return BOOTP_LEN;
}

int dhcp_parse_reply(const UINT8 *p, UINT32 len, UINT32 xid, dhcp_msg_t *m) {
    if (len < 240) return -1;
    if (p[0] != 2 || p[1] != 1 || p[2] != 6) return -1;       // BOOTREPLY / ethernet
    if (be32(p + 4) != xid) return -1;
    if (memcmp(p + 28, net_mac(), 6) != 0) return -1;         // baska istemciye ait
    if (be32(p + 236) != COOKIE) return -1;
    memset(m, 0, sizeof *m);
    memcpy(&m->yiaddr, p + 16, 4);
    UINT32 i = 240;
    while (i < len) {
        UINT8 code = p[i++];
        if (code == 0) continue;                              // pad
        if (code == 255) break;                               // end
        if (i >= len) return -1;
        UINT8 ol = p[i++];
        if (i + ol > len) return -1;                          // secenek paket sinirini asiyor
        const UINT8 *v = p + i;
        switch (code) {
        case 53: if (ol == 1) m->type = v[0]; break;
        case 1:  if (ol == 4) memcpy(&m->mask, v, 4); break;
        case 3:  if (ol >= 4) memcpy(&m->gw, v, 4); break;
        case 6:  if (ol >= 4) memcpy(&m->dns, v, 4); break;
        case 51: if (ol == 4) m->lease = be32(v); break;
        case 54: if (ol == 4) memcpy(&m->server, v, 4); break;
        default: break;
        }
        i += ol;
    }
    return m->type ? 0 : -1;
}

static void log_bound(const dhcp_msg_t *m) {
    char a[16], b[16], c[16], d[16];
    net_format_ip(m->yiaddr, a); net_format_ip(m->mask, b); net_format_ip(m->gw, c); net_format_ip(m->dns, d);
    kprintf("[dhcp] baglandi: IP %s maske %s gecit %s DNS %s, kira %u sn\n", a, b, c, d, m->lease);
}

static void bind_from(const dhcp_msg_t *m) {
    UINT32 mask = m->mask ? m->mask : IP4(255, 255, 255, 0);
    UINT32 lease = m->lease ? m->lease : 3600;
    net_apply_config(m->yiaddr, mask, m->gw, m->dns, m->server, lease, DHCP_BOUND);
    g_lease_s = lease;
    g_t1_s = lease == 0xFFFFFFFFu ? lease : lease / 2;
    g_t2_s = lease == 0xFFFFFFFFu ? lease : (UINT32)((UINT64)lease * 7 / 8);
    g_bound_tick = timer_get_ticks();
    g_renew_due = 0;
}

// Cevap bekler: istenen tur gelene (ya da NAK / zaman asimi) kadar. 0 = geldi, -1 = zaman asimi, -2 = NAK
static int wait_reply(int sock, UINT32 xid, int want, dhcp_msg_t *m, UINT32 ms) {
    UINT64 start = timer_get_ticks(), tmo = ms_to_ticks(ms);
    UINT8 buf[600];
    while (timer_get_ticks() - start < tmo) {
        UINT32 slice = 50;
        UINT32 from; UINT16 fport;
        int n = udp_recvfrom(sock, buf, sizeof buf, &from, &fport, slice);
        if (n < 0) continue;
        if (dhcp_parse_reply(buf, (UINT32)n, xid, m) != 0) continue;
        if (m->type == DHCP_NAK_T) return -2;
        if (m->type == want) return 0;
    }
    return -1;
}

int dhcp_acquire(UINT32 timeout_ms) {
    if (!net_present()) return -1;
    UINT64 f = irq_save();
    if (g_busy) { irq_restore(f); return -1; }
    g_busy = 1;
    irq_restore(f);

    int sock = udp_open(68);
    if (sock < 0) { g_busy = 0; return -1; }
    UINT64 start = timer_get_ticks(), total = ms_to_ticks(timeout_ms);
    int result = -1;
    UINT8 pkt[BOOTP_LEN];
    for (int attempt = 0; attempt < 6 && timer_get_ticks() - start < total && result != 0; attempt++) {
        UINT32 xid = (UINT32)rand64();
        dhcp_msg_t offer, ack;
        net_set_dhcp_state(DHCP_DISCOVER);
        build(pkt, DHCP_DISCOVER_T, xid, 0, 0, 0, 1);
        udp_sendto_from(sock, 0, 0xFFFFFFFFu, 67, pkt, BOOTP_LEN);
        UINT32 wait = 1500 + attempt * 500;
        if (wait_reply(sock, xid, DHCP_OFFER_T, &offer, wait) != 0) continue;

        net_set_dhcp_state(DHCP_REQUEST);
        build(pkt, DHCP_REQUEST_T, xid, 0, offer.yiaddr, offer.server, 1);
        udp_sendto_from(sock, 0, 0xFFFFFFFFu, 67, pkt, BOOTP_LEN);
        int r = wait_reply(sock, xid, DHCP_ACK_T, &ack, 2500);
        if (r == 0 && ack.yiaddr) {
            if (!ack.server) ack.server = offer.server;
            bind_from(&ack);
            log_bound(&ack);
            result = 0;
        }
    }
    udp_close(sock);
    if (result != 0) {
        net_set_dhcp_state(DHCP_FAILED);
        kprintf("[dhcp] sunucudan yanit alinamadi (mevcut yapilandirma korunuyor)\n");
    }
    g_busy = 0;
    return result;
}

// Kira yenileme: T1'den sonra sunucuya unicast, T2'den sonra yayin Request (ciaddr = mevcut IP).
static int renew_run(int broadcast) {
    net_status_t st; net_get_status(&st);
    if (!st.ip || !st.server) return -1;
    int sock = udp_open(68);
    if (sock < 0) return -1;
    net_set_dhcp_state(DHCP_RENEWING);
    UINT8 pkt[BOOTP_LEN];
    int ok = -1;
    for (int attempt = 0; attempt < 3 && ok != 0; attempt++) {
        UINT32 xid = (UINT32)rand64();
        dhcp_msg_t ack;
        build(pkt, DHCP_REQUEST_T, xid, st.ip, 0, 0, 0);
        udp_sendto_from(sock, st.ip, broadcast ? 0xFFFFFFFFu : st.server, 67, pkt, BOOTP_LEN);
        int r = wait_reply(sock, xid, DHCP_ACK_T, &ack, 2000);
        if (r == 0) {
            ack.yiaddr = st.ip;                               // yenilemede yiaddr dolu gelir; IP degismez
            if (!ack.server) ack.server = st.server;
            if (!ack.mask) ack.mask = st.mask;
            if (!ack.gw) ack.gw = st.gw;
            if (!ack.dns) ack.dns = st.dns;
            bind_from(&ack);
            ok = 0;
        } else if (r == -2) break;                            // NAK: bu IP artik gecersiz
    }
    udp_close(sock);
    if (ok != 0) net_set_dhcp_state(DHCP_BOUND);              // basarisiz: kira bitene kadar mevcut IP gecerli
    return ok;
}

void dhcp_force_renew(void) {
    UINT64 f = irq_save();
    if (g_busy) { irq_restore(f); return; }
    g_busy = 1; irq_restore(f);
    int r = renew_run(0);
    kprintf("[dhcp] kira yenileme (unicast): %s\n", r == 0 ? "basarili" : "basarisiz");
    g_busy = 0;
}

static void worker(void *arg) {
    int mode = (int)(UINTN)arg;               // 1: yenile, 2: kira doldu -> yeniden edin
    UINT64 f = irq_save();
    if (g_busy) { irq_restore(f); return; }
    g_busy = 1; irq_restore(f);
    if (mode == 1) {
        int after_t2 = (timer_get_ticks() - g_bound_tick) / TIMER_HZ >= g_t2_s;
        if (renew_run(after_t2) != 0) g_renew_due = 0;       // bir sonraki dhcp_tick yeniden dener (15 sn sonra)
    }
    g_busy = 0;
    if (mode == 2) dhcp_acquire(15000);
}

static UINT64 g_last_try;
void dhcp_tick(void) {
    net_status_t st;
    if (g_busy) return;
    net_get_status(&st);
    if (st.dhcp_state != DHCP_BOUND || g_lease_s == 0xFFFFFFFFu || !g_bound_tick) return;
    UINT64 now = timer_get_ticks();
    UINT32 el = (UINT32)((now - g_bound_tick) / TIMER_HZ);
    if (el >= g_lease_s) {                                    // kira doldu: IP'yi birak, yeniden edin
        kprintf("[dhcp] kira doldu; yapilandirma birakiliyor\n");
        net_apply_config(0, 0, 0, 0, 0, 0, DHCP_IDLE);
        g_bound_tick = 0;
        thread_create("dhcp", worker, (void *)(UINTN)2);
    } else if (el >= g_t1_s && now - g_last_try > 15 * TIMER_HZ) {
        g_last_try = now;
        thread_create("dhcp-renew", worker, (void *)(UINTN)1);
    }
}

static void boot_thread(void *arg) {
    (void)arg;
    dhcp_acquire(12000);
}

void dhcp_boot_start(void) {
    if (net_present()) thread_create("dhcp", boot_thread, 0);
}

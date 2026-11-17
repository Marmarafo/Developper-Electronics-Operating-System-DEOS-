// DEOS -- Ethernet suruculeri (Intel e1000/8254x, legacy descriptor modu, yoklama) + L2/L3: ARP, IPv4, ICMP.
// Ust katmanlar: udp.c (soketler), dhcp.c, dns.c, tcp.c (bkz. net.h, net_priv.h).
//
// Tasarim notlari:
//  - Kesme yok: "net" thread'i RX halkasini, TCP zamanlayicilarini ve DHCP kirasini ~5 ms'de bir isler.
//  - TX: her tanimlayicinin KENDI tamponu var (16 x 2 KB); gonderim tamamlanmayi beklemez (RS/DD ile geri alinir).
//  - Gelen IPv4 basliklari tam dogrulanir: surum, IHL, toplam uzunluk (cerceve sinirlari), baslik saglamasi,
//    parcalanmis paketler (reassembly yok) atilir, hedef adres filtrelenir.
//  - ip_output yalnizca KESMELER ACIKKEN ARP bekler (uyuyabilir); kesmeler kapaliyken (TCP kritik bolgeleri)
//    ARP istegi gonderip paketi atar, ust katman (TCP) yeniden dener.
#include "net.h"
#include "net_priv.h"
#include "types.h"
#include "pci.h"
#include "pmm.h"
#include "paging.h"
#include "io.h"
#include "string.h"
#include "kprintf.h"
#include "timer.h"
#include "thread.h"
#include "heap.h"
#include "rand.h"
#include "config.h"

// 8254x kaydedicileri
#define REG_CTRL   0x0000
#define REG_STATUS 0x0008
#define REG_EERD   0x0014
#define REG_RCTL   0x0100
#define REG_TCTL   0x0400
#define REG_RDBAL  0x2800
#define REG_RDBAH  0x2804
#define REG_RDLEN  0x2808
#define REG_RDH    0x2810
#define REG_RDT    0x2818
#define REG_TDBAL  0x3800
#define REG_TDBAH  0x3804
#define REG_TDLEN  0x3808
#define REG_TDH    0x3810
#define REG_TDT    0x3818
#define REG_MTA    0x5200
#define REG_RAL    0x5400
#define REG_RAH    0x5404

#define CTRL_RST   (1u << 26)
#define CTRL_SLU   (1u << 6)
#define CTRL_FD    0x01u

#define RCTL_EN    (1u << 1)
#define RCTL_BAM   (1u << 15)              // broadcast kabul
#define RCTL_SECRC (1u << 30)              // CRC'yi ayir

#define TCTL_EN    (1u << 1)
#define TCTL_PSP   (1u << 3)
#define TCTL_CT    (0x0Fu << 4)
#define TCTL_COLD  (0x40u << 12)

#define RX_N 128
#define TX_N 16
#define RX_BUF 2048
#define TX_BUF 2048

typedef struct { UINT64 addr; UINT16 len; UINT8 cso; UINT8 cmd; UINT8 status; UINT8 css; UINT16 special; } __attribute__((packed)) txd_t;
typedef struct { UINT64 addr; UINT16 len; UINT16 csum; UINT8 status; UINT8 errors; UINT16 vlan; } __attribute__((packed)) rxd_t;
_Static_assert(sizeof(txd_t) == 16, "txd_t 16 bayt olmali");
_Static_assert(sizeof(rxd_t) == 16, "rxd_t 16 bayt olmali");

#define TXCMD_EOP  0x01
#define TXCMD_IFCS 0x02
#define TXCMD_RS   0x08
#define RXST_DD    0x01

static volatile UINT8 *g_bar;
static UINT64 g_txring_phys, g_txbuf_phys, g_rxring_phys, g_rxbuf_phys;
static txd_t *g_txring;
static rxd_t *g_rxring;
static UINT8 *g_txbuf;
static UINT8  g_txbusy[TX_N];
static UINT32 g_txtail, g_rdt = RX_N - 1;
static volatile int g_rxlock;

static UINT8  g_mac[6];
static UINT32 g_ip   = IP4(10, 0, 2, 15);       // QEMU slirp varsayilani (DHCP basarili olursa degisir)
static UINT32 g_mask = IP4(255, 255, 255, 0);
static UINT32 g_gw   = IP4(10, 0, 2, 2);
static UINT32 g_dns  = IP4(10, 0, 2, 3);
static UINT32 g_server, g_lease;
static int    g_dhcp_state;
static net_stats_t g_stats;

static UINT32 rd32r(UINT32 o) { return *(volatile UINT32 *)(g_bar + o); }
static void   wr32r(UINT32 o, UINT32 v) { *(volatile UINT32 *)(g_bar + o) = v; }

static UINT32 ms_to_ticks(UINT32 ms) { UINT64 t = ((UINT64)ms * TIMER_HZ + 999) / 1000; return t ? (UINT32)t : 1; }
static int can_sleep(void) { UINT64 f; __asm__ __volatile__ ("pushfq; pop %0" : "=r"(f)); return (f >> 9) & 1; }

static int e1000_family(UINT16 dev) {
    switch (dev) {
    case 0x1004: case 0x1008: case 0x100C: case 0x100D: case 0x100E: case 0x100F:
    case 0x1010: case 0x1011: case 0x1012: case 0x1013: case 0x1014: case 0x1015:
    case 0x1016: case 0x1017: case 0x1018: case 0x1019: case 0x101A: case 0x101D:
    case 0x101E: case 0x1026: case 0x1027: case 0x1028:
    case 0x10C9: case 0x10CE: case 0x10D3: case 0x10EA:                  // e1000e
        return 1;
    default: return 0;
    }
}

static UINT16 eerd(UINT8 addr) {
    wr32r(REG_EERD, ((UINT32)addr << 8) | 1u);
    for (int i = 0; i < 100000; i++) if (rd32r(REG_EERD) & 2u) break;
    return (UINT16)(rd32r(REG_EERD) >> 16);
}

// ------------------------------------------------------------------ saglama toplami, adres yardimcilari
UINT16 net_csum16(const void *p, UINT32 n) {
    const UINT8 *b = (const UINT8 *)p;
    UINT32 s = 0, i;
    for (i = 0; i + 1 < n; i += 2) s += ((UINT32)b[i] << 8) | b[i + 1];
    if (n & 1) s += (UINT32)b[n - 1] << 8;
    while (s >> 16) s = (s & 0xFFFFu) + (s >> 16);
    return (UINT16)~s;
}

UINT16 l4_csum(UINT32 src, UINT32 dst, UINT8 proto, const void *seg, UINT32 len) {
    UINT8 ph[12];
    memcpy(ph, &src, 4); memcpy(ph + 4, &dst, 4);
    ph[8] = 0; ph[9] = proto; put16(ph + 10, (UINT16)len);
    UINT32 s = 0, i;
    for (i = 0; i < 12; i += 2) s += ((UINT32)ph[i] << 8) | ph[i + 1];
    const UINT8 *b = (const UINT8 *)seg;
    for (i = 0; i + 1 < len; i += 2) s += ((UINT32)b[i] << 8) | b[i + 1];
    if (len & 1) s += (UINT32)b[len - 1] << 8;
    while (s >> 16) s = (s & 0xFFFFu) + (s >> 16);
    return (UINT16)~s;
}

int net_parse_ip(const char *s, UINT32 *out) {
    UINT32 parts[4] = {0, 0, 0, 0};
    for (int i = 0; i < 4; i++) {
        UINT32 v = 0; int n = 0;
        while (s[0] >= '0' && s[0] <= '9') { v = v * 10 + (UINT32)(s[0] - '0'); if (++n > 3) return -1; s++; }
        if (n == 0 || v > 255) return -1;
        parts[i] = v;
        if (i < 3) { if (s[0] != '.') return -1; s++; }
    }
    while (*s == ' ') s++;                        // sondaki bosluklar serbest
    if (*s) return -1;                            // baska her sey (1.2.3.4.5, 1.2.3.4x ...) gecersiz
    *out = IP4(parts[0], parts[1], parts[2], parts[3]);
    return 0;
}

void net_format_ip(UINT32 ip, char out[16]) {
    ksnprintf(out, 16, "%u.%u.%u.%u", ip & 0xFF, (ip >> 8) & 0xFF, (ip >> 16) & 0xFF, (ip >> 24) & 0xFF);
}

int    net_present(void) { return g_bar != 0; }
UINT32 net_ip(void)      { return g_ip; }
UINT32 net_mask(void)    { return g_mask; }
UINT32 net_gw(void)      { return g_gw; }
UINT32 net_dns(void)     { return g_dns; }
const UINT8 *net_mac(void) { return g_mac; }

int net_is_broadcast(UINT32 ip) {
    if (ip == 0xFFFFFFFFu) return 1;
    return g_ip && g_mask && ip == ((g_ip & g_mask) | ~g_mask);
}

// Bu paket bize mi? Unicast (kendi IP'miz), yayin ya da henuz IP'miz yokken (DHCP) herhangi bir adres.
static int for_us(UINT32 dst) {
    if (g_dhcp_state == DHCP_DISCOVER || g_dhcp_state == DHCP_REQUEST) return 1;     // sunucu yaniti yiaddr'a unicast olabilir
    return g_ip == 0 || dst == g_ip || net_is_broadcast(dst);
}

// ------------------------------------------------------------------ TX / RX halkalari
static int net_send_raw(const void *data, UINT32 len) {
    if (!g_bar || len < 14 || len > TX_BUF) { g_stats.tx_dropped++; return -1; }
    for (int spin = 0; spin < 20000; spin++) {
        UINT64 f = irq_save();
        UINT32 slot = g_txtail;
        txd_t *d = &g_txring[slot];
        if (!g_txbusy[slot] || (*(volatile UINT8 *)&d->status & RXST_DD)) {     // tanimlayici bos (ya da tamamlandi)
            memcpy(g_txbuf + (UINTN)slot * TX_BUF, data, len);
            d->addr = g_txbuf_phys + (UINT64)slot * TX_BUF;
            d->len = (UINT16)len; d->cso = 0; d->css = 0; d->special = 0;
            d->cmd = TXCMD_EOP | TXCMD_IFCS | TXCMD_RS;
            *(volatile UINT8 *)&d->status = 0;
            g_txbusy[slot] = 1;
            g_txtail = (slot + 1) % TX_N;
            g_stats.tx_frames++;
            __asm__ __volatile__ ("mfence" ::: "memory");
            wr32r(REG_TDT, g_txtail);
            irq_restore(f);
            return 0;
        }
        irq_restore(f);
        __asm__ __volatile__ ("pause");
    }
    g_stats.tx_dropped++;
    return -1;
}

// ------------------------------------------------------------------ ARP
#define ARP_N 8
#define ARP_TTL_S 600
typedef struct { UINT32 ip; UINT8 mac[6]; UINT64 when; int valid; } arp_t;
static arp_t g_arp[ARP_N];

static void cache_add(UINT32 ip, const UINT8 *mac) {
    if (!ip) return;
    UINT64 f = irq_save();
    int slot = -1;
    for (int i = 0; i < ARP_N; i++) if (g_arp[i].valid && g_arp[i].ip == ip) slot = i;
    if (slot < 0) {
        slot = 0;
        for (int i = 0; i < ARP_N; i++) {
            if (!g_arp[i].valid) { slot = i; break; }
            if (g_arp[i].when < g_arp[slot].when) slot = i;
        }
    }
    g_arp[slot].ip = ip; memcpy(g_arp[slot].mac, mac, 6);
    g_arp[slot].when = timer_get_ticks(); g_arp[slot].valid = 1;
    irq_restore(f);
}

static int cache_find(UINT32 ip, UINT8 *mac_out) {
    UINT64 f = irq_save();
    int ok = 0;
    UINT64 now = timer_get_ticks();
    for (int i = 0; i < ARP_N; i++) {
        if (g_arp[i].valid && g_arp[i].ip == ip) {
            if (now - g_arp[i].when > (UINT64)ARP_TTL_S * TIMER_HZ) g_arp[i].valid = 0;     // eskidi
            else { memcpy(mac_out, g_arp[i].mac, 6); ok = 1; }
            break;
        }
    }
    irq_restore(f);
    return ok;
}

static void arp_send(UINT16 op, const UINT8 *dst_mac, const UINT8 *tha, UINT32 tpa) {
    UINT8 f[60];
    memset(f, 0, sizeof f);
    memcpy(f, dst_mac, 6); memcpy(f + 6, g_mac, 6);
    put16(f + 12, 0x0806);
    put16(f + 14, 1); put16(f + 16, 0x0800); f[18] = 6; f[19] = 4; put16(f + 20, op);
    memcpy(f + 22, g_mac, 6); memcpy(f + 28, &g_ip, 4);
    if (tha) memcpy(f + 32, tha, 6);
    memcpy(f + 38, &tpa, 4);
    net_send_raw(f, 60);
}

static void arp_send_request(UINT32 target) {
    static const UINT8 bc[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    g_stats.arp_requests++;
    arp_send(1, bc, 0, target);
}

static void arp_input(const UINT8 *f, UINT32 len) {
    if (len < 42) return;
    if (be16(f + 14) != 1 || be16(f + 16) != 0x0800 || f[18] != 6 || f[19] != 4) return;   // yalnizca Ethernet/IPv4
    UINT16 op = be16(f + 20);
    UINT32 spa, tpa; memcpy(&spa, f + 28, 4); memcpy(&tpa, f + 38, 4);
    const UINT8 *sha = f + 22;
    if (op == 1 && g_ip && tpa == g_ip) {                     // bize soruyor: yanitla (+ soran'i ogren)
        cache_add(spa, sha);
        arp_send(2, sha, sha, spa);
    } else if (op == 2) {                                     // yanit
        cache_add(spa, sha);
    }
}

static int arp_resolve(UINT32 ip, UINT8 *mac, UINT32 timeout_ms) {
    UINT64 start = timer_get_ticks(), last = 0;
    UINT64 tmo = ms_to_ticks(timeout_ms), gap = ms_to_ticks(250);
    int sent = 0;
    for (;;) {
        if (cache_find(ip, mac)) return 0;
        UINT64 now = timer_get_ticks();
        if (now - start >= tmo) return -1;
        if (!sent || now - last >= gap) { arp_send_request(ip); last = now; sent = 1; }
        net_poll_once();
        thread_sleep_ms(2);
    }
}

// ------------------------------------------------------------------ IPv4 cikis
int ip_output(UINT32 src, UINT32 dst, UINT8 proto, const void *payload, UINT32 len) {
    if (!g_bar || len > NET_MTU - 20) return -1;
    UINT8 dmac[6];
    if (net_is_broadcast(dst)) {
        memset(dmac, 0xFF, 6);
    } else {
        if (g_ip == 0 || dst == g_ip) return -1;              // IP'miz yok (yalniz yayin) ya da kendimize
        UINT32 nh = dst;
        if ((dst & g_mask) != (g_ip & g_mask)) { if (!g_gw) return -1; nh = g_gw; }
        if (!cache_find(nh, dmac)) {
            if (!can_sleep()) { arp_send_request(nh); return -1; }
            if (arp_resolve(nh, dmac, 1500) != 0) return -1;
        }
    }
    UINT8 pkt[14 + NET_MTU];
    memcpy(pkt, dmac, 6); memcpy(pkt + 6, g_mac, 6);
    put16(pkt + 12, 0x0800);
    UINT8 *ip = pkt + 14;
    static UINT16 ipid;
    UINT64 fl = irq_save(); UINT16 id = ++ipid; irq_restore(fl);
    ip[0] = 0x45; ip[1] = 0;
    put16(ip + 2, (UINT16)(20 + len)); put16(ip + 4, id); put16(ip + 6, 0x4000);     // DF
    ip[8] = 64; ip[9] = proto; ip[10] = ip[11] = 0;
    memcpy(ip + 12, &src, 4); memcpy(ip + 16, &dst, 4);
    put16(ip + 10, net_csum16(ip, 20));
    memcpy(ip + 20, payload, len);
    return net_send_raw(pkt, 14 + 20 + len);
}

// Hedefe giden ilk sekmenin MAC'ini cozer (ARP bekler; uyuyabilir). TCP baglantisi ilk SYN'den ONCE bunu cagirir.
int net_resolve_nexthop(UINT32 ip, UINT32 timeout_ms) {
    if (!g_bar || net_is_broadcast(ip)) return 0;
    if (g_ip == 0 || ip == g_ip) return -1;
    UINT32 nh = ip;
    if ((ip & g_mask) != (g_ip & g_mask)) { if (!g_gw) return -1; nh = g_gw; }
    UINT8 mac[6];
    if (cache_find(nh, mac)) return 0;
    return arp_resolve(nh, mac, timeout_ms);
}

// ------------------------------------------------------------------ ICMP
#define PING_ID 0xDE0A
static UINT16 g_ping_seq;
static volatile int g_ping_got;
static volatile UINT64 g_ping_tick;

static void icmp_input(UINT32 src, UINT32 dst, const UINT8 *m, UINT32 len) {
    if (len < 8 || net_csum16(m, len) != 0) return;           // kisa ya da saglama hatali
    if (m[0] == 0) {                                           // echo reply
        if (be16(m + 4) == PING_ID && be16(m + 6) == g_ping_seq) { g_ping_tick = timer_get_ticks(); g_ping_got = 1; }
        return;
    }
    if (m[0] != 8 || m[1] != 0) return;                        // yalniz echo request
    g_stats.icmp_echo_in++;
    if (net_is_broadcast(dst) || len > NET_MTU - 20) return;   // yayin ping'ine yanit verme
    UINT8 r[NET_MTU - 20];
    memcpy(r, m, len);
    r[0] = 0; r[2] = r[3] = 0;
    put16(r + 2, net_csum16(r, len));
    ip_output(g_ip, src, IPPROTO_ICMP, r, len);
}

int net_ping_ip(UINT32 ip, UINT32 timeout_ms) {
    if (!g_bar) return -1;
    UINT8 m[8 + 32];
    memset(m, 0, sizeof m);
    m[0] = 8; put16(m + 4, PING_ID);
    for (int i = 0; i < 32; i++) m[8 + i] = (UINT8)('a' + (i % 23));
    g_ping_seq++;
    put16(m + 6, g_ping_seq);
    put16(m + 2, net_csum16(m, sizeof m));
    g_ping_got = 0;
    UINT64 start = timer_get_ticks();
    if (ip_output(g_ip, ip, IPPROTO_ICMP, m, sizeof m) != 0) return -1;
    UINT64 tmo = ms_to_ticks(timeout_ms);
    while (timer_get_ticks() - start < tmo) {
        net_poll_once();
        if (g_ping_got) return (int)((g_ping_tick - start) * 1000 / TIMER_HZ);
        thread_sleep_ms(2);
    }
    return -1;
}

int net_ping(const char *ipstr) {
    if (!g_bar) { kprintf("Ethernet: NIC yok\n"); return -1; }
    UINT32 target;
    if (net_parse_ip(ipstr, &target)) { kprintf("Kullanim: ping A.B.C.D\n"); return -1; }
    int rtt = net_ping_ip(target, 3000);
    if (rtt < 0) { kprintf("ping: %s zaman asimi / ulasilamadi\n", ipstr); return -1; }
    kprintf("%d bayt: %s seq=%d, sure=%d ms\n", 8 + 32, ipstr, g_ping_seq, rtt);
    return 0;
}

// ------------------------------------------------------------------ IPv4 giris
static void ipv4_input(const UINT8 *f, UINT32 len) {
    if (len < 14 + 20) { g_stats.rx_dropped++; return; }
    const UINT8 *ip = f + 14;
    UINT32 avail = len - 14;
    if ((ip[0] >> 4) != 4) { g_stats.rx_dropped++; return; }
    UINT32 ihl = (UINT32)(ip[0] & 15) * 4;
    if (ihl < 20 || ihl > avail) { g_stats.rx_dropped++; return; }
    UINT32 tot = be16(ip + 2);
    if (tot < ihl || tot > avail) { g_stats.rx_dropped++; return; }            // cerceve disina tasan toplam uzunluk
    if (net_csum16(ip, ihl) != 0) { g_stats.rx_dropped++; return; }            // baslik saglamasi
    if (be16(ip + 6) & 0x3FFF) { g_stats.rx_dropped++; return; }               // MF / parca ofseti: reassembly yok
    UINT32 src, dst; memcpy(&src, ip + 12, 4); memcpy(&dst, ip + 16, 4);
    if (!for_us(dst)) { g_stats.rx_dropped++; return; }
    if (src && g_ip && ((src & g_mask) == (g_ip & g_mask)) && !net_is_broadcast(src)) cache_add(src, f + 6);   // gonderenin MAC'ini taze tut
    const UINT8 *pl = ip + ihl; UINT32 plen = tot - ihl;
    switch (ip[9]) {
    case IPPROTO_ICMP: icmp_input(src, dst, pl, plen); break;
    case IPPROTO_UDP:  udp_input(src, dst, pl, plen); break;
    case IPPROTO_TCP:  tcp_input(src, dst, pl, plen); break;
    default: break;
    }
}

static void net_handle(const UINT8 *f, UINT32 len) {
    if (len < 14) { g_stats.rx_dropped++; return; }
    int to_me = memcmp(f, g_mac, 6) == 0;
    int bcast = (f[0] & f[1] & f[2] & f[3] & f[4] & f[5]) == 0xFF;
    if (!to_me && !bcast) return;
    UINT16 et = be16(f + 12);
    if (et == 0x0806) arp_input(f, len);
    else if (et == 0x0800) ipv4_input(f, len);
}

void net_poll_once(void) {
    if (!g_bar) return;
    while (__sync_lock_test_and_set(&g_rxlock, 1)) __asm__ __volatile__ ("pause");
    for (;;) {
        UINT32 i = (g_rdt + 1) % RX_N;
        if (i == (rd32r(REG_RDH) % RX_N)) break;
        rxd_t *d = &g_rxring[i];
        if (!(*(volatile UINT8 *)&d->status & RXST_DD)) break;
        UINT32 len = d->len;
        UINT8 *buf = (UINT8 *)(UINTN)d->addr;
        g_stats.rx_frames++;
        if (!d->errors && len >= 14 && len <= RX_BUF) net_handle(buf, len); else g_stats.rx_dropped++;
        d->status = 0;
        g_rdt = i;
        wr32r(REG_RDT, i);
    }
    __sync_lock_release(&g_rxlock);
}

void net_rx_inject(const UINT8 *frame, UINT32 len) {
    if (len < 14 || len > RX_BUF) return;
    UINT8 *copy = kmalloc(len);
    if (!copy) return;
    memcpy(copy, frame, len);
    while (__sync_lock_test_and_set(&g_rxlock, 1)) __asm__ __volatile__ ("pause");
    net_handle(copy, len);
    __sync_lock_release(&g_rxlock);
    kfree(copy);
}

static void net_thread(void *arg) {
    (void)arg;
    for (;;) { net_poll_once(); tcp_tick(); dhcp_tick(); thread_sleep_ms(5); }
}

// ------------------------------------------------------------------ yapilandirma / bilgi
const char *net_mac_str(void) {
    static char buf[18];
    for (int i = 0; i < 6; i++) {
        buf[i * 3]     = "0123456789abcdef"[g_mac[i] >> 4];
        buf[i * 3 + 1] = "0123456789abcdef"[g_mac[i] & 15];
        buf[i * 3 + 2] = (i < 5) ? ':' : 0;
    }
    return buf;
}

void net_apply_config(UINT32 ip, UINT32 mask, UINT32 gw, UINT32 dns, UINT32 server, UINT32 lease_s, int st) {
    UINT64 f = irq_save();
    g_ip = ip; g_mask = mask; g_gw = gw; g_dns = dns; g_server = server; g_lease = lease_s; g_dhcp_state = st;
    for (int i = 0; i < ARP_N; i++) g_arp[i].valid = 0;
    irq_restore(f);
}
void net_set_dhcp_state(int st) { g_dhcp_state = st; }

int net_set_config(UINT32 ip, UINT32 mask, UINT32 gw, UINT32 dns) {
    net_apply_config(ip, mask, gw, dns, 0, 0, DHCP_IDLE);
    return 0;
}

void net_get_status(net_status_t *o) {
    memset(o, 0, sizeof *o);
    o->ip = g_ip; o->mask = g_mask; o->gw = g_gw; o->dns = g_dns; o->server = g_server; o->lease_s = g_lease;
    o->dhcp_state = g_dhcp_state;
    o->link_up = g_bar ? ((rd32r(REG_STATUS) & 2) != 0) : 0;
    memcpy(o->mac, g_mac, 6);
}
void net_get_stats(net_stats_t *o) { *o = g_stats; }

void net_info(void) {
    kprintf("Ethernet: %s\n", g_bar ? "Intel e1000 uyumlu" : "NIC yok");
    if (!g_bar) return;
    char a[16], b[16], c[16], d[16];
    net_format_ip(g_ip, a); net_format_ip(g_mask, b); net_format_ip(g_gw, c); net_format_ip(g_dns, d);
    kprintf("  MAC: %s   baglanti: %s\n  IP: %s  maske: %s  ag gecidi: %s  DNS: %s\n",
            net_mac_str(), (rd32r(REG_STATUS) & 2) ? "UP" : "DOWN/kuruluyor", a, b, c, d);
    kprintf("  DHCP: %s", dhcp_state_name(g_dhcp_state));
    if (g_server) { net_format_ip(g_server, a); kprintf(" (sunucu %s, kira %u sn)", a, g_lease); }
    kprintf("\n  cerceve: alinan %llu (atilan %llu), gonderilen %llu (atilan %llu), ARP istegi %llu\n",
            g_stats.rx_frames, g_stats.rx_dropped, g_stats.tx_frames, g_stats.tx_dropped, g_stats.arp_requests);
}

// 'netcfg A.B.C.D [maske|/N] [gecit]'
int net_set_ip(const char *args) {
    char tok[3][24]; int nt = 0;
    const char *p = args;
    while (*p && nt < 3) {
        while (*p == ' ') p++;
        if (!*p) break;
        int n = 0;
        while (*p && *p != ' ' && n < 23) tok[nt][n++] = *p++;
        tok[nt][n] = 0; nt++;
    }
    UINT32 ip, mask = IP4(255, 255, 255, 0), gw;
    if (nt < 1 || net_parse_ip(tok[0], &ip)) { kprintf("Kullanim: netcfg A.B.C.D [maske veya /N] [gecit]\n"); return -1; }
    if (nt >= 2) {
        if (tok[1][0] == '/') {
            int bits = 0; const char *q = tok[1] + 1;
            while (*q >= '0' && *q <= '9') bits = bits * 10 + (*q++ - '0');
            if (*q || bits < 0 || bits > 32) { kprintf("netcfg: gecersiz onek uzunlugu\n"); return -1; }
            mask = bits ? IP4((0xFFFFFFFFu << (32 - bits)) >> 24, (0xFFFFFFFFu << (32 - bits)) >> 16, (0xFFFFFFFFu << (32 - bits)) >> 8, (0xFFFFFFFFu << (32 - bits))) : 0;
        } else if (net_parse_ip(tok[1], &mask)) { kprintf("netcfg: gecersiz maske\n"); return -1; }
    }
    if (nt >= 3) { if (net_parse_ip(tok[2], &gw)) { kprintf("netcfg: gecersiz gecit\n"); return -1; } }
    else { gw = (ip & mask) | IP4(2, 0, 0, 0); if (gw == ip) gw = (ip & mask) | IP4(1, 0, 0, 0); }       // slirp varsayimi: .2
    net_set_config(ip, mask, gw, g_dns);
    char a[16], b[16], c[16]; net_format_ip(ip, a); net_format_ip(mask, b); net_format_ip(gw, c);
    kprintf("Yerel IP: %s, maske %s, ag gecidi %s\n", a, b, c);
    return 0;
}

// ------------------------------------------------------------------ baslatma
int net_init(void) {
    const pci_dev_t *d = 0;
    for (int i = 0; i < pci_count(); i++) {
        const pci_dev_t *c = pci_get(i);
        if (c->class_code == 0x02 && c->subclass == 0x00 && c->vendor == 0x8086 && e1000_family(c->device)) { d = c; break; }
    }
    if (!d) return 0;
    pci_enable(d);

    UINT32 lo = pci_read32(d, 0x10);
    if (lo & 1) { kprintf("[net] BAR0 I/O tipi (destek yok)\n"); return 0; }
    UINT64 bar = (UINT64)(lo & ~0xFULL);
    if ((lo & 0x6) == 0x4) bar |= (UINT64)pci_read32(d, 0x14) << 32;
    if (!bar) { kprintf("[net] BAR0 atanmamis\n"); return 0; }
    volatile UINT8 *regs = (volatile UINT8 *)mmio_map(bar, 0x20000);       // 128 KB; 4 GB ustu BAR'lar da eslenir
    if (!regs) { kprintf("[net] BAR eslenemedi\n"); return 0; }
    g_bar = regs;

    // Reset + link up
    wr32r(REG_CTRL, CTRL_RST);
    thread_sleep_ms(5);
    wr32r(REG_CTRL, CTRL_SLU | CTRL_FD);
    for (int i = 0; i < 300 && !(rd32r(REG_STATUS) & 2); i++) thread_sleep_ms(10);

    // MAC
    UINT32 ral = rd32r(REG_RAL), rah = rd32r(REG_RAH);
    if (rah & 0x80000000u) {
        g_mac[0] = ral & 0xFF; g_mac[1] = (ral >> 8) & 0xFF; g_mac[2] = (ral >> 16) & 0xFF; g_mac[3] = (ral >> 24) & 0xFF;
        g_mac[4] = rah & 0xFF; g_mac[5] = (rah >> 8) & 0xFF;
    } else {
        UINT16 w0 = eerd(0), w1 = eerd(1), w2 = eerd(2);
        g_mac[0] = w0 & 0xFF; g_mac[1] = w0 >> 8; g_mac[2] = w1 & 0xFF;
        g_mac[3] = w1 >> 8; g_mac[4] = w2 & 0xFF; g_mac[5] = w2 >> 8;
    }
    for (int i = 0; i < 128; i++) wr32r(REG_MTA + i * 4, 0);

    // TX: 16 tanimlayici, her biri icin 2 KB tampon (DMA < 4 GB)
    g_txring_phys = pmm_alloc_pages(1); g_txbuf_phys = pmm_alloc_pages(TX_N * TX_BUF / 4096);
    if (!g_txring_phys || !g_txbuf_phys || g_txring_phys >= 0x100000000ULL || g_txbuf_phys >= 0x100000000ULL) { g_bar = 0; return 0; }
    memset((void *)(UINTN)g_txring_phys, 0, 4096);
    g_txring = (txd_t *)(UINTN)g_txring_phys; g_txbuf = (UINT8 *)(UINTN)g_txbuf_phys;
    wr32r(REG_TDBAL, (UINT32)g_txring_phys); wr32r(REG_TDBAH, 0);
    wr32r(REG_TDLEN, TX_N * 16); wr32r(REG_TDH, 0); wr32r(REG_TDT, 0);
    g_txtail = 0;

    // RX: 128 tanimlayici + 128 x 2048 B tampon
    g_rxring_phys = pmm_alloc_pages(1); g_rxbuf_phys = pmm_alloc_pages(RX_N * RX_BUF / 4096);
    if (!g_rxring_phys || !g_rxbuf_phys || g_rxring_phys >= 0x100000000ULL || g_rxbuf_phys >= 0x100000000ULL) { g_bar = 0; return 0; }
    memset((void *)(UINTN)g_rxring_phys, 0, 4096);
    g_rxring = (rxd_t *)(UINTN)g_rxring_phys;
    for (int i = 0; i < RX_N; i++) g_rxring[i].addr = g_rxbuf_phys + (UINT64)i * RX_BUF;
    wr32r(REG_RDBAL, (UINT32)g_rxring_phys); wr32r(REG_RDBAH, 0);
    wr32r(REG_RDLEN, RX_N * 16); wr32r(REG_RDH, 0);
    g_rdt = RX_N - 1; wr32r(REG_RDT, g_rdt);

    wr32r(REG_TCTL, TCTL_EN | TCTL_PSP | TCTL_CT | TCTL_COLD);
    wr32r(REG_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC);

    thread_create("net", net_thread, 0);
    kprintf("[net] e1000 dev=0x%x MAC %s link=%s\n", d->device, net_mac_str(), (rd32r(REG_STATUS) & 2) ? "UP" : "DOWN");
    return 1;
}

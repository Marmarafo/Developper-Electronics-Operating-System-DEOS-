// DEOS -- Ethernet suruculeru: Intel e1000/8254x (legacy descriptor modu, yoklama).
// Kesme yok: bir kernel thread RX kuyrugunu 10 ms'de bir isler. DMA icin <4GB
// fiziksel/birebir eslenmis tamponlar kullanilir (sistem geri kalaniyla ayni sinir).
#include "net.h"
#include "types.h"
#include "pci.h"
#include "pmm.h"
#include "io.h"
#include "string.h"
#include "kprintf.h"
#include "timer.h"
#include "thread.h"

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

typedef struct { UINT64 addr; UINT16 len; UINT8 cso; UINT8 cmd; UINT8 status; UINT8 css; UINT16 special; } __attribute__((packed)) txd_t;
typedef struct { UINT64 addr; UINT16 len; UINT16 csum; UINT8 status; UINT8 errors; UINT16 vlan; } __attribute__((packed)) rxd_t;
typedef char _c1[(sizeof(txd_t) == 16) ? 1 : -1];
typedef char _c2[(sizeof(rxd_t) == 16) ? 1 : -1];

#define TXCMD_EOP  0x01
#define TXCMD_IFCS 0x02
#define TXCMD_RS   0x08
#define RXST_DD    0x01
#define RXST_EOP   0x02

static volatile UINT8 *g_bar;
static UINT64 g_txring_phys, g_rxring_phys, g_rxbuf_phys, g_txbuf_phys;
static txd_t *g_txring;
static rxd_t *g_rxring;
static UINT8 *g_txbuf;
static UINT32 g_rdt = RX_N - 1;
static volatile int g_rxlock, g_txlock;
void net_notify_reply(void);   // icmp_handle icin ileri bildirim

static UINT8  g_mac[6];
static UINT32 g_ip   = 0x0A00020F;   // 10.0.2.15  (QEMU slirp varsayilani)
static UINT32 g_gw   = 0x0A000202;   // 10.0.2.2
static UINT16 g_ping_seq = 0;
static volatile int g_got_reply;
static volatile int g_dhcp_state;  // 0=idle, 1=discover, 2=request, 3=bound
static UINT32 g_dhcp_xid = 0xDE0A0000;
static UINT32 g_reply_ms;

typedef struct { UINT32 ip; UINT8 mac[6]; UINT64 when; int valid; } arp_t;
static arp_t g_arp[4];

static UINT32 rd32r(UINT32 o) { return *(volatile UINT32 *)(g_bar + o); }
static void   wr32r(UINT32 o, UINT32 v) { *(volatile UINT32 *)(g_bar + o) = v; }

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

// ---------------- IPv4/ICMP/ARP yardimcilar ----------------
static UINT16 csum16(const void *p, UINT32 n) {
    const UINT8 *b = (const UINT8 *)p;
    UINT32 s = 0;
    UINT32 i;
    for (i = 0; i + 1 < n; i += 2) s += ((UINT32)b[i] << 8) | b[i + 1];
    if (n & 1) s += (UINT32)b[n - 1] << 8;
    while (s >> 16) s = (s & 0xFFFFu) + (s >> 16);
    return (UINT16)~s;
}

static void cache_add(UINT32 ip, const UINT8 *mac) {
    int slot = -1;
    for (int i = 0; i < 4; i++) if (g_arp[i].valid && g_arp[i].ip == ip) slot = i;
    if (slot < 0) { slot = 0; for (int i = 1; i < 4; i++) if (!g_arp[i].valid || g_arp[i].when < g_arp[slot].when) slot = i; }
    g_arp[slot].ip = ip; memcpy(g_arp[slot].mac, mac, 6);
    g_arp[slot].when = timer_get_ticks(); g_arp[slot].valid = 1;
}
static const UINT8 *cache_find(UINT32 ip) {
    for (int i = 0; i < 4; i++) if (g_arp[i].valid && g_arp[i].ip == ip) return g_arp[i].mac;
    return 0;
}

static int net_send_raw(const void *data, UINT32 len) {
    if (len < 14 || len > RX_BUF) return -1;
    while (__sync_lock_test_and_set(&g_txlock, 1)) __asm__ __volatile__("pause");
    memcpy(g_txbuf, data, len);
    txd_t *d = &g_txring[0];
    d->addr = g_txbuf_phys; d->len = (UINT16)len;
    d->cso = 0; d->cmd = TXCMD_EOP | TXCMD_IFCS | TXCMD_RS; d->status = 0; d->css = 0; d->special = 0;
    wr32r(REG_TDT, 1);
    UINT64 start = timer_get_ticks();
    int ok = 0;
    while (timer_get_ticks() - start < 100) {
        if (d->status & RXST_DD) { ok = 1; break; }   // DD biti TX icin de ayni 0x01
        __asm__ __volatile__("pause");
    }
    __sync_lock_release(&g_txlock);
    return ok ? 0 : -1;
}

static void arp_request(UINT32 target) {
    UINT8 f[64];
    memset(f, 0, sizeof f);
    memset(f, 0xFF, 6);                          // dst broadcast
    memcpy(f + 6, g_mac, 6);                     // src
    f[12] = 0x08; f[13] = 0x06;                  // EtherType ARP
    f[14] = 0; f[15] = 1;                        // HTYPE ethernet
    f[16] = 0x08; f[17] = 0x00;                  // PTYPE ipv4
    f[18] = 6; f[19] = 4;                        // HLEN, PLEN
    f[20] = 0; f[21] = 1;                        // OPER = request
    memcpy(f + 22, g_mac, 6);                    // SHA
    memcpy(f + 28, &g_ip, 4);                    // SPA
    /* THA sifir */ memcpy(f + 38, &target, 4);  // TPA
    net_send_raw(f, 42);
}

// Gelen cerceve isleyicileri
static void arp_handle(const UINT8 *f, UINT32 len) {
    if (len < 42) return;
    UINT16 oper = ((UINT16)f[20] << 8) | f[21];
    UINT32 spa; memcpy(&spa, f + 28, 4);
    const UINT8 *sha = f + 22;
    const UINT8 *tpa = f + 38;
    if (oper == 1 && memcmp(tpa, &g_ip, 4) == 0) {           // bize soruyor: yanitla
        UINT8 r[42];
        memcpy(r, sha, 6); memcpy(r + 6, g_mac, 6);
        r[12] = 0x08; r[13] = 0x06;
        r[14] = 0; r[15] = 1; r[16] = 0x08; r[17] = 0;
        r[18] = 6; r[19] = 4; r[20] = 0; r[21] = 2;
        memcpy(r + 22, g_mac, 6); memcpy(r + 28, &g_ip, 4);
        memcpy(r + 32, sha, 6); memcpy(r + 38, &spa, 4);
        net_send_raw(r, 42);
    }
    if (oper == 2) cache_add(spa, sha);                       // yanit: onbellege al
}

static void icmp_handle(UINT8 *f, UINT32 len) {
    if (len < 14 + 20 + 8) return;
    UINT8 *ip = f + 14;
    UINT32 ihl = (ip[0] & 0x0F) * 4;
    if (ihl < 20) return;
    UINT8 *icmp = f + 14 + ihl;
    UINT32 tot; memcpy(&tot, ip + 2, 2); tot = ((tot & 0xFF) << 8) | (tot >> 8);
    if (tot > len - 14) tot = len - 14;

    if (icmp[0] == 0) {                                       // echo reply -> ping bekleyicisini uyandir
        net_notify_reply();
        return;
    }
    if (icmp[0] != 8) return;                                 // yalnizca echo request ile ilgilen
    {
        // Echo reply olustur: MAC+IP swap, type 0, csum yeniden
        UINT8 tmp[6]; memcpy(tmp, f, 6); memcpy(f, f + 6, 6); memcpy(f + 6, tmp, 6);
        memcpy(tmp, ip + 12, 4); memcpy(ip + 12, ip + 16, 4); memcpy(ip + 16, tmp, 4);
        ip[8] = 64;
        ip[10] = ip[11] = 0; UINT16 c = csum16(ip, ihl); ip[10] = (UINT8)(c >> 8); ip[11] = (UINT8)c;
        icmp[0] = 0; icmp[2] = icmp[3] = 0;
        UINT32 icmplen = tot - ihl;
        c = csum16(icmp, icmplen); icmp[2] = (UINT8)(c >> 8); icmp[3] = (UINT8)c;
        net_send_raw(f, 14 + tot);
    }
}

static int g_udp_ready;
static UINT16 g_udp_port;
static UINT32 g_udp_from;
static UINT16 g_udp_from_port;
static UINT8  g_udp_buf[1536];
static UINT32 g_udp_len;

static void udp_handle(UINT8 *f, UINT32 len) {
    if (len < 14 + 20 + 8) return;
    UINT8 *ip = f + 14;
    UINT32 ihl = (ip[0] & 0x0F) * 4;
    if (ihl < 20) return;
    UINT8 *udp = f + 14 + ihl;
    UINT16 dst_port = ((UINT16)udp[2] << 8) | udp[3];
    if (dst_port == g_udp_port) {
        UINT16 udplen = ((UINT16)udp[4] << 8) | udp[5];
        if (udplen < 8) return;
        g_udp_from = 0; memcpy(&g_udp_from, ip + 12, 4);
        g_udp_from_port = ((UINT16)udp[0] << 8) | udp[1];
        g_udp_len = udplen - 8;
        if (g_udp_len > sizeof g_udp_buf) g_udp_len = sizeof g_udp_buf;
        memcpy(g_udp_buf, udp + 8, g_udp_len);
        g_udp_ready = 1;
    }
}

static void ipv4_handle(UINT8 *f, UINT32 len) {
    UINT8 *ip = f + 14;
    UINT32 ihl = (ip[0] & 0x0F) * 4;
    if (ihl < 20 || len < 14 + ihl) return;
    if (ip[9] == 17) { udp_handle(f, len); return; }         // UDP
    if (ip[9] != 1) return;                                   // ICMP disini simdilik yoksay
    UINT32 tot; memcpy(&tot, ip + 2, 2); tot = ((tot & 0xFF) << 8) | (tot >> 8);
    icmp_handle(f, len);
}


static void net_handle(UINT8 *f, UINT32 len) {
    if (len < 14) return;
    if (memcmp(f, g_mac, 6) != 0 && !(f[0] == 0xFF && f[1] == 0xFF)) return;
    UINT16 et = ((UINT16)f[12] << 8) | f[13];
    if (et == 0x0806) arp_handle(f, len);
    else if (et == 0x0800) ipv4_handle(f, len);
}

void net_poll_once(void) {
    while (__sync_lock_test_and_set(&g_rxlock, 1)) __asm__ __volatile__("pause");
    for (;;) {
        UINT32 i = (g_rdt + 1) % RX_N;
        if (i == (rd32r(REG_RDH) % RX_N)) break;
        rxd_t *d = &g_rxring[i];
        if (!(d->status & RXST_DD)) break;
        UINT32 len = d->len;
        UINT8 *buf = (UINT8 *)(UINTN)d->addr;
        if (!(d->errors) && len >= 14 && len <= RX_BUF) net_handle(buf, len);
        d->status = 0;
        g_rdt = i;
        wr32r(REG_RDT, i);
    }
    __sync_lock_release(&g_rxlock);
}

static void net_thread(void *arg) {
    (void)arg;
    for (;;) { net_poll_once(); thread_sleep_ms(10); }
}

// ---------------- dis API ----------------
const char *net_mac_str(void) {
    static char buf[18];
    for (int i = 0; i < 6; i++) {
        buf[i * 3]     = "0123456789abcdef"[g_mac[i] >> 4];
        buf[i * 3 + 1] = "0123456789abcdef"[g_mac[i] & 15];
        buf[i * 3 + 2] = (i < 5) ? ':' : 0;
    }
    return buf;
}

void net_info(void) {
    kprintf("Ethernet: %s\n", g_bar ? "Intel e1000 uyumlu" : "NIC yok");
    if (!g_bar) return;
    kprintf("  MAC: %s\n  IP:  %d.%d.%d.%d\n  GW:  %d.%d.%d.%d\n  Baglanti: %s\n",
            net_mac_str(), g_ip & 0xFF, (g_ip >> 8) & 0xFF, (g_ip >> 16) & 0xFF, (g_ip >> 24) & 0xFF,
            g_gw & 0xFF, (g_gw >> 8) & 0xFF, (g_gw >> 16) & 0xFF, (g_gw >> 24) & 0xFF,
            (rd32r(REG_STATUS) & 2) ? "UP" : "DOWN/kuruluyor");
}

static int parse_ip(const char *s, UINT32 *out) {
    UINT32 parts[4] = {0, 0, 0, 0};
    for (int i = 0; i < 4; i++) {
        UINT32 v = 0; int n = 0;
        while (s[0] >= '0' && s[0] <= '9') { v = v * 10 + (UINT32)(s[0] - '0'); if (++n > 3) return -1; s++; }
        if (n == 0 || v > 255) return -1;
        parts[i] = v;
        if (i < 3) { if (s[0] != '.') return -1; s++; }
    }
    *out = parts[0] | (parts[1] << 8) | (parts[2] << 16) | (parts[3] << 24);
    return 0;
}

int net_set_ip(const char *s) {
    UINT32 ip;
    if (parse_ip(s, &ip)) { kprintf("Kullanim: netcfg A.B.C.D\n"); return -1; }
    g_ip = ip;
    g_gw = (ip & 0x00FFFFFFu) | 0x02u;
    if (g_gw == ip) g_gw = (ip & 0x00FFFFFFu) | 0x01u;
    for (int i = 0; i < 4; i++) g_arp[i].valid = 0;
    kprintf("Yerel IP: %d.%d.%d.%d, GW varsayimi: %d.%d.%d.%d\n",
            ip & 0xFF, (ip >> 8) & 0xFF, (ip >> 16) & 0xFF, (ip >> 24) & 0xFF,
            g_gw & 0xFF, (g_gw >> 8) & 0xFF, (g_gw >> 16) & 0xFF, (g_gw >> 24) & 0xFF);
    return 0;
}

int net_ping(const char *ipstr) {
    if (!g_bar) { kprintf("Ethernet: NIC yok\n"); return -1; }
    UINT32 target;
    if (parse_ip(ipstr, &target)) { kprintf("Kullanim: ping A.B.C.D\n"); return -1; }
    const UINT8 *dmac = cache_find(target);
    for (int t = 0; t < 3 && !dmac; t++) {
        arp_request(target);
        UINT64 start = timer_get_ticks();
        while (timer_get_ticks() - start < 100) { net_poll_once(); thread_sleep_ms(5); }
        dmac = cache_find(target);
    }
    if (!dmac) { kprintf("ping: %s icin ARP yaniti gelmedi\n", ipstr); return -1; }

    UINT8 f[64];
    memcpy(f, dmac, 6); memcpy(f + 6, g_mac, 6);
    f[12] = 0x08; f[13] = 0x00;
    UINT8 *ip = f + 14;
    ip[0] = 0x45; ip[1] = 0;
    UINT16 tot = 20 + 8;
    ip[2] = (UINT8)(tot >> 8); ip[3] = (UINT8)tot;
    ip[4] = 0; ip[5] = (UINT8)g_ping_seq;
    ip[6] = 0; ip[7] = 0;
    ip[8] = 64; ip[9] = 1;
    ip[10] = ip[11] = 0;
    memcpy(ip + 12, &g_ip, 4); memcpy(ip + 16, &target, 4);
    UINT16 c = csum16(ip, 20); ip[10] = (UINT8)(c >> 8); ip[11] = (UINT8)c;
    UINT8 *icmp = ip + 20;
    icmp[0] = 8; icmp[1] = 0; icmp[2] = icmp[3] = 0;
    icmp[4] = 0xDE; icmp[5] = 0x0A; icmp[6] = 0; icmp[7] = (UINT8)g_ping_seq;
    c = csum16(icmp, 8); icmp[2] = (UINT8)(c >> 8); icmp[3] = (UINT8)c;

    g_got_reply = 0;
    g_ping_seq++;
    if (net_send_raw(f, 14 + tot)) { kprintf("ping: gonderme basarisiz\n"); return -1; }
    UINT64 start = timer_get_ticks();
    while (timer_get_ticks() - start < 300) {
        net_poll_once();
        if (g_got_reply) { kprintf("%d bayt: %s seq=%d, sure=%u ms\n", tot, ipstr, g_ping_seq - 1, (UINT32)(g_reply_ms - start)); return 0; }
        thread_sleep_ms(10);
    }
    kprintf("ping: %s zaman asimi (3 sn)\n", ipstr);
    return -1;
}

// RX icindeki echo-reply yakalama (net_handle -> ipv4 -> icmp icinde cagrilir)
void net_notify_reply(void) { g_reply_ms = (UINT32)timer_get_ticks(); g_got_reply = 1; }

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
    if (!bar || bar >= 0x100000000ULL) { kprintf("[net] BAR >= 4GB\n"); return 0; }
    g_bar = (volatile UINT8 *)(UINTN)bar;

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

    // MTA temizle
    for (int i = 0; i < 128; i++) wr32r(REG_MTA + i * 4, 0);

    // TX: 16 descriptor
    g_txring_phys = pmm_alloc_pages(1); g_txbuf_phys = pmm_alloc_pages(1);
    if (!g_txring_phys || !g_txbuf_phys) return 0;
    memset((void *)(UINTN)g_txring_phys, 0, 4096);
    g_txring = (txd_t *)(UINTN)g_txring_phys; g_txbuf = (UINT8 *)(UINTN)g_txbuf_phys;
    wr32r(REG_TDBAL, (UINT32)g_txring_phys); wr32r(REG_TDBAH, 0);
    wr32r(REG_TDLEN, TX_N * 16); wr32r(REG_TDH, 0); wr32r(REG_TDT, 0);

    // RX: 128 descriptor + 128x2048B tampon
    g_rxring_phys = pmm_alloc_pages(1); g_rxbuf_phys = pmm_alloc_pages(RX_N * RX_BUF / 4096);
    if (!g_rxring_phys || !g_rxbuf_phys) return 0;
    memset((void *)(UINTN)g_rxring_phys, 0, 4096);
    g_rxring = (rxd_t *)(UINTN)g_rxring_phys;
    for (int i = 0; i < RX_N; i++) g_rxring[i].addr = g_rxbuf_phys + (UINT64)i * RX_BUF;
    wr32r(REG_RDBAL, (UINT32)g_rxring_phys); wr32r(REG_RDBAH, 0);
    wr32r(REG_RDLEN, RX_N * 16); wr32r(REG_RDH, 0);
    g_rdt = RX_N - 1; wr32r(REG_RDT, g_rdt);

    // RX/TX ac
    wr32r(REG_TCTL, TCTL_EN | TCTL_PSP | TCTL_CT | TCTL_COLD);
    wr32r(REG_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC);

    thread_create("net", net_thread, 0);
    kprintf("[net] e1000 dev=0x%x MAC %s link=%s\n", d->device, net_mac_str(), (rd32r(REG_STATUS) & 2) ? "UP" : "DOWN");
    return 1;
}

// ---- UDP ----
int net_udp_send(UINT32 dst_ip, UINT16 dst_port, UINT16 src_port, const void *data, UINT32 len) {
    if (!g_bar || len > 1400) return -1;
    const UINT8 *dmac = cache_find(dst_ip);
    if (!dmac) { arp_request(dst_ip); thread_sleep_ms(50); dmac = cache_find(dst_ip); if (!dmac) return -1; }
    UINT8 pkt[1536];
    memcpy(pkt, dmac, 6); memcpy(pkt + 6, g_mac, 6);
    pkt[12] = 0x08; pkt[13] = 0x00;
    UINT8 *ip = pkt + 14;
    ip[0] = 0x45; ip[1] = 0;
    UINT16 tot = 20 + 8 + len;
    ip[2] = (UINT8)(tot >> 8); ip[3] = (UINT8)tot;
    static UINT16 ipid; ipid++;
    ip[4] = (UINT8)(ipid >> 8); ip[5] = (UINT8)ipid;
    ip[6] = 0; ip[7] = 0;
    ip[8] = 64; ip[9] = 17;  // UDP
    ip[10] = ip[11] = 0;
    memcpy(ip + 12, &g_ip, 4); memcpy(ip + 16, &dst_ip, 4);
    UINT16 c = csum16(ip, 20); ip[10] = (UINT8)(c >> 8); ip[11] = (UINT8)c;
    UINT8 *udp = ip + 20;
    udp[0] = (UINT8)(src_port >> 8); udp[1] = (UINT8)src_port;
    udp[2] = (UINT8)(dst_port >> 8); udp[3] = (UINT8)dst_port;
    UINT16 udplen = 8 + len;
    udp[4] = (UINT8)(udplen >> 8); udp[5] = (UINT8)udplen;
    udp[6] = udp[7] = 0;
    memcpy(udp + 8, data, len);
    // UDP pseudo-header checksum
    UINT8 ph[12]; memcpy(ph, ip + 12, 4); memcpy(ph + 4, ip + 16, 4);
    ph[8] = 0; ph[9] = 17; ph[10] = (UINT8)(udplen >> 8); ph[11] = (UINT8)udplen;
    UINT32 cs = 0;
    for (int i = 0; i < 12; i += 2) cs += ((UINT32)ph[i] << 8) | ph[i+1];
    for (int i = 0; i < udplen; i += 2) cs += ((UINT32)udp[i] << 8) | (i+1 < udplen ? udp[i+1] : 0);
    while (cs >> 16) cs = (cs & 0xFFFF) + (cs >> 16);
    c = (UINT16)~cs; if (c == 0) c = 0xFFFF;
    udp[6] = (UINT8)(c >> 8); udp[7] = (UINT8)c;
    return net_send_raw(pkt, 14 + tot);
}


int net_udp_recv(UINT16 port, void *buf, UINT32 max, UINT32 *from_ip, UINT16 *from_port, UINT32 timeout_ms) {
    g_udp_port = port; g_udp_ready = 0;
    UINT64 start = timer_get_ticks();
    while (timer_get_ticks() - start < timeout_ms) {
        net_poll_once();
        if (g_udp_ready && g_udp_port == port) {
            UINT32 n = g_udp_len < max ? g_udp_len : max;
            memcpy(buf, g_udp_buf, n);
            if (from_ip) *from_ip = g_udp_from;
            if (from_port) *from_port = g_udp_from_port;
            g_udp_ready = 0;
            return (int)n;
        }
        thread_sleep_ms(10);
    }
    return -1;
}

// RX icinde UDP yakalama


// ---- DHCP Client (sadece Discover/Offer/Request/Ack dongusu) ----
void net_dhcp_request(void) {
    if (!g_bar) return;
    g_dhcp_state = 1;
    UINT8 pkt[512];
    memset(pkt, 0, sizeof pkt);
    pkt[0] = 1;                // BOOTREQUEST
    pkt[1] = 1;                // HTYPE ethernet
    pkt[2] = 6;                // HLEN
    pkt[3] = 0;                // HOPS
    pkt[4] = (UINT8)(g_dhcp_xid >> 24); pkt[5] = (UINT8)(g_dhcp_xid >> 16);
    pkt[6] = (UINT8)(g_dhcp_xid >> 8);  pkt[7] = (UINT8)g_dhcp_xid;
    memcpy(pkt + 28, g_mac, 6);
    // DHCP Magic Cookie
    pkt[236] = 99; pkt[237] = 130; pkt[238] = 83; pkt[239] = 99;
    // Options
    int o = 240;
    pkt[o++] = 53; pkt[o++] = 1; pkt[o++] = 1;  // DHCP Discover
    pkt[o++] = 55; pkt[o++] = 3; pkt[o++] = 1; pkt[o++] = 3; pkt[o++] = 6;  // Param request
    pkt[o++] = 255;  // END

    UINT32 broadcast = 0xFFFFFFFF;
    net_udp_send(broadcast, 67, 68, pkt, 300);
    kprintf("[dhcp] Discover gonderildi\n");
}
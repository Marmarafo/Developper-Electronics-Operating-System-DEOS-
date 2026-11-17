// DEOS -- ag yigini ic ortak tanimlar (yalnizca net.c, udp.c, dhcp.c, dns.c, tcp.c kullanir).
#ifndef DEOS_NET_PRIV_H
#define DEOS_NET_PRIV_H
#include "net.h"
#include "cpu.h"

#define IPPROTO_ICMP 1
#define IPPROTO_TCP  6
#define IPPROTO_UDP  17

static inline UINT16 be16(const UINT8 *p) { return (UINT16)((p[0] << 8) | p[1]); }
static inline UINT32 be32(const UINT8 *p) { return ((UINT32)p[0] << 24) | ((UINT32)p[1] << 16) | ((UINT32)p[2] << 8) | p[3]; }
static inline void   put16(UINT8 *p, UINT16 v) { p[0] = (UINT8)(v >> 8); p[1] = (UINT8)v; }
static inline void   put32(UINT8 *p, UINT32 v) { p[0] = (UINT8)(v >> 24); p[1] = (UINT8)(v >> 16); p[2] = (UINT8)(v >> 8); p[3] = (UINT8)v; }

// Kisa kritik bolge (kesmeler kapali). Ic ice kullanilabilir.
#define NET_CRIT_BEGIN()  UINT64 _crit_flags = irq_save()
#define NET_CRIT_END()    irq_restore(_crit_flags)

// ---- net.c -> alt protokoller ----
int    ip_output(UINT32 src, UINT32 dst, UINT8 proto, const void *payload, UINT32 len);   // 0 | -1
UINT16 l4_csum(UINT32 src, UINT32 dst, UINT8 proto, const void *seg, UINT32 len);          // sozde baslikli; dogru pakette 0 doner
int    net_is_broadcast(UINT32 ip);
int    net_resolve_nexthop(UINT32 ip, UINT32 timeout_ms);   // ARP'i onceden coz (uyuyabilir): 0 | -1
UINT32 net_ip(void);
UINT32 net_mask(void);
UINT32 net_gw(void);
UINT32 net_dns(void);
const UINT8 *net_mac(void);
void   net_apply_config(UINT32 ip, UINT32 mask, UINT32 gw, UINT32 dns, UINT32 server, UINT32 lease_s, int dhcp_state);
void   net_set_dhcp_state(int st);

int    udp_sendto_from(int s, UINT32 src, UINT32 ip, UINT16 port, const void *data, UINT32 len);   // kaynak IP'yi belirler (DHCP: 0.0.0.0)

// ---- alt protokol giris noktalari (net.c cagirir; IP basligi dogrulanmis, seg/len = IP yuku) ----
void udp_input(UINT32 src, UINT32 dst, const UINT8 *seg, UINT32 len);
void tcp_input(UINT32 src, UINT32 dst, const UINT8 *seg, UINT32 len);
void tcp_tick(void);
void dhcp_tick(void);

#endif

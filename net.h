// DEOS -- Ag yigini (genel API): Intel e1000 suruculeri, ARP, IPv4, ICMP, UDP soketleri, DHCP, DNS, TCP istemcisi.
//
// IPv4 adresleri UINT32'dir ve ILK OKTET DUSUK BAYTTADIR (bellekteki ag sirasi): 10.0.2.15 = IP4(10,0,2,15).
// Yigin yoklama tabanlidir (kesme yok): "net" thread'i RX halkasini, TCP zamanlayicilarini ve DHCP kirasini
// yaklasik 5 ms'de bir isler; bekleyen islevler de gerektiginde RX'i kendileri isler.
#ifndef DEOS_NET_H
#define DEOS_NET_H
#include "types.h"

#define IP4(a, b, c, d) ((UINT32)(a) | ((UINT32)(b) << 8) | ((UINT32)(c) << 16) | ((UINT32)(d) << 24))
#define NET_MTU 1500

enum { DHCP_IDLE = 0, DHCP_DISCOVER, DHCP_REQUEST, DHCP_BOUND, DHCP_RENEWING, DHCP_FAILED };

typedef struct {
    UINT32 ip, mask, gw, dns;      // 0 = yok
    UINT32 server, lease_s;        // DHCP sunucusu ve kira suresi (sn); statik ayarda 0
    int    dhcp_state;
    int    link_up;
    UINT8  mac[6];
} net_status_t;

typedef struct { UINT64 rx_frames, rx_dropped, tx_frames, tx_dropped, arp_requests, icmp_echo_in; } net_stats_t;

// ---- surucu / yapilandirma ----
int  net_init(void);                                  // 1 = NIC hazir
int  net_present(void);
void net_get_status(net_status_t *out);
void net_get_stats(net_stats_t *out);
void net_info(void);                                  // 'netinfo'
int  net_set_ip(const char *args);                    // 'netcfg A.B.C.D [maske] [gecit]'; 0 | -1
int  net_set_config(UINT32 ip, UINT32 mask, UINT32 gw, UINT32 dns);
const char *net_mac_str(void);
void net_poll_once(void);                             // RX kuyrugunu isle
void net_rx_inject(const UINT8 *frame, UINT32 len);   // test: cerceve "alinmis" gibi islenir

// ---- yardimcilar ----
int    net_parse_ip(const char *s, UINT32 *out);      // "A.B.C.D" -> out; 0 = gecerli
void   net_format_ip(UINT32 ip, char out[16]);
UINT16 net_csum16(const void *p, UINT32 n);           // Internet saglama toplami (RFC 1071)

// ---- ICMP ----
int  net_ping(const char *ip);                        // konsola yazar; 0 = yanit, -1 = yok
int  net_ping_ip(UINT32 ip, UINT32 timeout_ms);       // RTT (ms, >= 0) | -1

// ---- UDP soketleri (udp.c) ----
#define UDP_MAX_PAYLOAD 1472
int  udp_open(UINT16 port);                           // port 0 = gecici port; soket no | -1
void udp_close(int s);
int  udp_connect(int s, UINT32 ip, UINT16 port);      // varsayilan es (send/recv ve filtre)
int  udp_sendto(int s, UINT32 ip, UINT16 port, const void *data, UINT32 len);   // 0 | -1
int  udp_send(int s, const void *data, UINT32 len);   // connect'li soketten
int  udp_recvfrom(int s, void *buf, UINT32 max, UINT32 *from_ip, UINT16 *from_port, UINT32 timeout_ms);   // bayt | -1 zaman asimi | -2 gecersiz
int  udp_local_port(int s);
UINT32 udp_dropped(int s);

// ---- DHCP (dhcp.c) ----
typedef struct { int type; UINT32 yiaddr, mask, gw, dns, server, lease; } dhcp_msg_t;     // type: 1 discover, 2 offer, 3 request, 5 ack, 6 nak
int  dhcp_parse_reply(const UINT8 *p, UINT32 len, UINT32 xid, dhcp_msg_t *m);   // BOOTP/DHCP yaniti dogrular+ayristirir; 0 | -1 (test icin acik)
int  dhcp_acquire(UINT32 timeout_ms);                 // bloklar; 0 = BOUND, -1 = basarisiz
void dhcp_boot_start(void);                           // acilista arka planda dhcp_acquire
int  dhcp_state(void);
void dhcp_force_renew(void);                          // test: kirayi simdi yenile (bloklar)
const char *dhcp_state_name(int st);

// ---- DNS (dns.c) ----
int  dns_resolve(const char *name, UINT32 *ip, UINT32 timeout_ms);     // 0 | -1; "A.B.C.D" dogrudan ayristirilir
int  dns_resolve_via(const char *name, UINT32 *ip, UINT32 timeout_ms, UINT32 server, UINT16 port);   // belirli sunucu/port (testler)
int  dns_build_query(UINT16 id, const char *name, UINT8 *out, UINT32 cap);   // uzunluk | -1  (test icin acik)
int  dns_parse_response(const UINT8 *p, UINT32 len, UINT16 id, UINT32 *ip);  // 0 | -1 gecersiz | -2 ad yok (NXDOMAIN) | -3 A kaydi yok

// ---- TCP istemcisi (tcp.c): yalnizca aktif acilis (connect); listen/accept yok ----
#define TCP_ERR_TIMEOUT (-1)
#define TCP_ERR_RESET   (-2)
#define TCP_ERR_ARG     (-3)
#define TCP_ERR_NOMEM   (-4)
#define TCP_ERR_CLOSED  (-5)
int  tcp_connect(UINT32 ip, UINT16 port, UINT32 timeout_ms);           // el (>= 0) | hata (< 0)
int  tcp_write(int h, const void *data, UINT32 len, UINT32 timeout_ms);// yazilan bayt | hata
int  tcp_read(int h, void *buf, UINT32 max, UINT32 timeout_ms);        // bayt (>0) | 0 = karsi taraf kapatti | TCP_ERR_*
int  tcp_close(int h);                                                  // gonderilenleri bosaltip FIN yollar
int  tcp_state(int h);                                                  // test/tanilama
const char *tcp_state_name(int st);
int  tcp_conn_count(void);                                              // kullanimdaki baglanti sayisi

#endif

#include "types.h"
// DEOS -- Ethernet (Intel e1000/8254x ailesi) + minimal yigin: ARP, IPv4, ICMP echo.
#ifndef DEOS_NET_H
#define DEOS_NET_H

int  net_init(void);            // 1 = NIC hazir
int  net_ping(const char *ip);  // 0 = yanit alindi, -1 = zaman asimi/ARP yok
void net_info(void);            // MAC/IP/link durumu
int  net_set_ip(const char *ip);// "A.C.D" -> yerel IP; ag gecidi /24 icin .2 varsayilir
const char *net_mac_str(void);
int  net_udp_send(UINT32 dst_ip, UINT16 dst_port, UINT16 src_port, const void *data, UINT32 len);
int  net_udp_recv(UINT16 port, void *buf, UINT32 max, UINT32 *from_ip, UINT16 *from_port, UINT32 timeout_ms);
void net_dhcp_request(void);  // DHCP Discover gonder
void net_poll_once(void);       // RX kuyrugunu isle (net thread + ping)

#endif

// ifconfig: ag arayuzu bilgisi (IP, maske, ag gecidi, DNS, MAC, DHCP durumu).
#include "ulib.h"
int main(int argc, char **argv) {
    (void)argc; (void)argv;
    unetinfo_t ni;
    if (netinfo(&ni) < 0) { fputs("ifconfig: ag bilgisi alinamadi\n", stderr); return 1; }
    char ip[16], mask[16], gw[16], dns[16], srv[16];
    format_ip(ni.ip, ip); format_ip(ni.mask, mask); format_ip(ni.gw, gw); format_ip(ni.dns, dns); format_ip(ni.server, srv);
    static const char *st[] = { "yok (statik)", "kesif", "istek", "baglandi", "yenileniyor", "basarisiz" };
    printf("eth0: MAC %02x:%02x:%02x:%02x:%02x:%02x   baglanti: %s\n", ni.mac[0], ni.mac[1], ni.mac[2], ni.mac[3], ni.mac[4], ni.mac[5], ni.link_up ? "UP" : "DOWN");
    printf("      IP %s  maske %s  gecit %s  DNS %s\n", ip, mask, gw, dns);
    printf("      DHCP: %s", ni.dhcp_state <= 5 ? st[ni.dhcp_state] : "?");
    if (ni.server) printf(" (sunucu %s, kira %u sn)", srv, ni.lease);
    puts("");
    return 0;
}

// nc [-u] [-w sn] [-s "metin"] ana_makine port: basit netcat istemcisi.
//   Gonderilecek veri: -s ile verilen metin, yoksa stdin'in TAMAMI (EOF'a kadar; boru/yonlendirme ile kullanin).
//   Veri gonderildikten sonra, -w saniye (varsayilan 2) boyunca gelen yanitlar stdout'a yazilir.
//   Ornek:  echo merhaba | nc 10.0.2.2 7007     nc -u -s ping 10.0.2.2 7007
#include "ulib.h"
int main(int argc, char **argv) {
    int udp = 0, wait_s = 2, i = 1;
    const char *text = 0;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (!strcmp(argv[i], "-u")) udp = 1;
        else if (!strcmp(argv[i], "-w") && i + 1 < argc) wait_s = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-s") && i + 1 < argc) text = argv[++i];
        else { fprintf(stderr, "nc: bilinmeyen secenek %s\n", argv[i]); return 2; }
    }
    if (argc - i != 2) { fputs("kullanim: nc [-u] [-w sn] [-s metin] ana_makine port\n", stderr); return 2; }
    u32 ip; int port = atoi(argv[i + 1]);
    if (resolve(argv[i], &ip) < 0) { fprintf(stderr, "nc: %s cozulemedi\n", argv[i]); return 1; }
    if (port <= 0 || port > 65535) { fputs("nc: gecersiz port\n", stderr); return 2; }
    i64 fd = socket(udp ? SOCK_DGRAM : SOCK_STREAM);
    if (fd < 0) { fprintf(stderr, "nc: soket: %s\n", u_strerror(fd)); return 1; }
    i64 r = connect((int)fd, ip, (u32)port);
    if (r < 0) { fprintf(stderr, "nc: baglanti: %s\n", r == ECONNREFUSED_RET ? "reddedildi" : r == ETIMEDOUT_RET ? "zaman asimi" : u_strerror(r)); return 1; }
    if (text) { if (write_all((int)fd, text, strlen(text)) < 0) { fputs("nc: gonderilemedi\n", stderr); return 1; } }
    else {
        char buf[1024]; i64 n;
        while ((n = sys_read(0, buf, sizeof buf)) > 0) if (write_all((int)fd, buf, (size_t)n) < 0) { fputs("nc: gonderilemedi\n", stderr); return 1; }
    }
    sockopt((int)fd, SO_RCVTIMEO, (u32)wait_s * 1000);
    char rb[1024];
    for (;;) {
        i64 n = recvfrom((int)fd, rb, sizeof rb, 0);
        if (n <= 0) break;
        sys_write(1, rb, (u64)n);
    }
    close((int)fd);
    return 0;
}

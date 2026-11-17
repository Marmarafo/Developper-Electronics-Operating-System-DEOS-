// nettest <yanki-portu>: ring 3 soket API'sinin uctan uca testi (UDP + TCP, QEMU slirp uzerinden 10.0.2.2'deki
// yanki sunucusuna; bkz. ci/echo_server.py). Cikis kodu: 0 = hepsi gecti.
#include "ulib.h"

static int g_total, g_failed;
static void check(int cond, const char *name) { g_total++; if (!cond) g_failed++; printf("  [%s] %s\n", cond ? "PASS" : "FAIL", name); }

int main(int argc, char **argv) {
    int port = argc > 1 ? atoi(argv[1]) : 47007;
    u32 srv = IP4(10, 0, 2, 2);
    puts("-- ring 3 soket API --");

    unetinfo_t ni;
    check(netinfo(&ni) == 0 && ni.ip != 0 && ni.link_up, "netinfo: IP atanmis, baglanti UP");
    u32 ip = 0;
    check(resolve("10.1.2.3", &ip) == 0 && ip == IP4(10, 1, 2, 3), "resolve: A.B.C.D dogrudan");
    char nb[16]; format_ip(IP4(192, 168, 7, 9), nb); check(!strcmp(nb, "192.168.7.9"), "format_ip");

    // ---- hata yollari ----
    check(socket(99) == EINVAL_RET, "socket(gecersiz tur) -> EINVAL");
    i64 file = open("/", O_RDONLY | O_DIRECTORY);
    usockaddr_t sa = { srv, (unsigned short)port, 0 };
    check(file >= 0 && sendto((int)file, "x", 1, &sa) == ENOTSOCK_RET, "sendto(dosya fd) -> ENOTSOCK");
    if (file >= 0) close((int)file);
    check(sendto(15, "x", 1, &sa) == EBADF_RET, "sendto(acik olmayan fd) -> EBADF");
    i64 u1 = socket(SOCK_DGRAM), u2 = socket(SOCK_DGRAM);
    check(u1 >= 0 && u2 >= 0 && bind((int)u1, 5555) == 0 && bind((int)u2, 5555) == EADDRINUSE_RET, "bind: ayni port ikinci kez -> EADDRINUSE");
    check(recvfrom((int)u2, nb, sizeof nb, 0) == EINVAL_RET, "recvfrom(bind/sendto yapilmamis UDP) -> EINVAL");
    close((int)u1); close((int)u2);
    i64 t0 = socket(SOCK_STREAM);
    check(t0 >= 0 && sendto((int)t0, "x", 1, 0) == ENOTCONN_RET, "TCP: baglanmadan gonderme -> ENOTCONN");
    close((int)t0);

    // ---- UDP ----
    i64 us = socket(SOCK_DGRAM);
    sockopt((int)us, SO_RCVTIMEO, 3000);
    static u8 tx[1500], rx[1600];
    for (int i = 0; i < 1500; i++) tx[i] = (u8)(i * 11 + 5);
    int udp_ok = us >= 0;
    static const int sizes[] = { 1, 64, 700, 1472 };
    for (int k = 0; k < 4 && udp_ok; k++) {
        usockaddr_t from;
        i64 s = sendto((int)us, tx, (u64)sizes[k], &sa);
        i64 r = s == sizes[k] ? recvfrom((int)us, rx, sizeof rx, &from) : -1;
        if (r != sizes[k] || memcmp(rx, tx, (size_t)sizes[k]) || from.ip != srv || from.port != port) { udp_ok = 0; printf("    UDP %d bayt: s=%lld r=%lld\n", sizes[k], s, r); }
    }
    check(udp_ok, "UDP sendto/recvfrom: 1/64/700/1472 bayt yanki birebir, gonderen adres dogru");
    check(sendto((int)us, tx, 1473, &sa) == EINVAL_RET, "UDP: 1473 bayt -> EINVAL");
    check(connect((int)us, srv, (u32)port) == 0 && sys_write((int)us, "abc", 3) == 3, "UDP connect + write()");
    i64 rn = sys_read((int)us, rx, sizeof rx);
    check(rn == 3 && !memcmp(rx, "abc", 3), "UDP connect + read(): yanit alindi");
    sockopt((int)us, SO_RCVTIMEO, 300);
    u64 t1 = sys_ticks();
    check(recvfrom((int)us, rx, sizeof rx, 0) == ETIMEDOUT_RET && sys_ticks() - t1 < 150, "UDP: SO_RCVTIMEO (300 ms) -> ETIMEDOUT, makul sure");
    close((int)us);

    // ---- TCP: 100000 bayt, 2000'er bayt gidis-donus ----
    i64 ts = socket(SOCK_STREAM);
    i64 cr = ts >= 0 ? connect((int)ts, srv, (u32)port) : -1;
    check(cr == 0, "TCP connect");
    if (cr == 0) {
        sockopt((int)ts, SO_RCVTIMEO, 5000);
        enum { BIG = 100000, CH = 2000 };
        u8 *big = malloc(BIG), *back = malloc(BIG);
        int ok = big && back;
        if (ok) {
            for (int i = 0; i < BIG; i++) big[i] = (u8)(i * 131 + (i >> 8));
            u64 t2 = sys_ticks();
            for (int off = 0; off < BIG && ok; off += CH) {
                if (sys_write((int)ts, big + off, CH) != CH) { ok = 0; break; }
                int got = 0;
                while (got < CH) { i64 n = sys_read((int)ts, back + off + got, (u64)(CH - got)); if (n <= 0) { ok = 0; break; } got += (int)n; }
            }
            printf("    TCP akis: %d bayt, %llu ms\n", BIG, (sys_ticks() - t2) * 10);
            if (ok) ok = !memcmp(big, back, BIG);
        }
        check(ok, "TCP: 100000 bayt write()/read() yanki birebir ayni");
        free(big); free(back);
        i64 r = sendto((int)ts, "son", 3, 0);
        i64 r2 = recvfrom((int)ts, rx, sizeof rx, 0);
        check(r == 3 && r2 == 3 && !memcmp(rx, "son", 3), "TCP sendto/recvfrom (adressiz)");
    }
    if (ts >= 0) close((int)ts);

    // ---- reddedilen baglanti ----
    i64 tc = socket(SOCK_STREAM);
    i64 e = connect((int)tc, srv, 1);
    check(e == ECONNREFUSED_RET || e == ETIMEDOUT_RET || e == ECONNRESET_RET, "TCP: dinlenmeyen porta connect hata verir");
    close((int)tc);

    // ---- acik soketle cikis: cekirdek kaynaklari serbest birakmali (kernel testi tcp_conn_count ile dogrular) ----
    i64 leak = socket(SOCK_STREAM);
    if (leak >= 0 && connect((int)leak, srv, (u32)port) == 0) puts("    (soket bilerek acik birakiliyor)");

    printf("nettest: %d/%d gecti\n", g_total - g_failed, g_total);
    return g_failed ? 1 : 0;
}

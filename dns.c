// DEOS -- DNS istemcisi (RFC 1035, yalniz UDP, A kaydi). Sorgu olusturma ve yanit ayristirma ayri islevlerdir
// (net.h'de acik): bozuk/hileli yanitlara karsi tum okumalar sinir denetimlidir; sikistirma isaretcileri
// takip EDILMEZ (adlar yalnizca atlanir), bu yuzden dongu riski yoktur.
#include "net.h"
#include "config.h"
#include "net_priv.h"
#include "types.h"
#include "string.h"
#include "timer.h"
#include "rand.h"

int dns_build_query(UINT16 id, const char *name, UINT8 *out, UINT32 cap) {
    if (!name || !*name || cap < 12 + 2 + 4) return -1;
    UINT32 o = 12;
    put16(out, id); put16(out + 2, 0x0100);                   // RD (ozyinelemeli istek)
    put16(out + 4, 1); put16(out + 6, 0); put16(out + 8, 0); put16(out + 10, 0);
    const char *p = name;
    UINT32 total = 0;
    while (*p) {
        const char *q = p;
        while (*q && *q != '.') {
            char ch = *q;
            int ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_';
            if (!ok) return -1;
            q++;
        }
        UINT32 l = (UINT32)(q - p);
        if (l == 0 || l > 63) return -1;                      // bos etiket (".." / bastaki nokta) ya da cok uzun
        if (o + 1 + l + 1 + 4 > cap) return -1;
        out[o++] = (UINT8)l;
        memcpy(out + o, p, l); o += l;
        total += l + 1;
        if (total > 253) return -1;
        p = q;
        if (*p == '.') { p++; if (!*p) break; }               // sondaki nokta serbest
    }
    if (o + 1 + 4 > cap) return -1;
    out[o++] = 0;                                             // kok
    put16(out + o, 1); put16(out + o + 2, 1); o += 4;         // QTYPE=A, QCLASS=IN
    return (int)o;
}

// p[*pos]'daki adi atlar (sikistirma isaretcisi 2 bayt). 0 | -1 (bozuk/sinir disi)
static int skip_name(const UINT8 *p, UINT32 len, UINT32 *pos) {
    for (int guard = 0; guard < 130; guard++) {
        if (*pos >= len) return -1;
        UINT8 c = p[*pos];
        if (c == 0) { (*pos)++; return 0; }
        if ((c & 0xC0) == 0xC0) { if (*pos + 2 > len) return -1; *pos += 2; return 0; }
        if (c & 0xC0) return -1;                              // 0x40 / 0x80 etiket turleri gecersiz
        if (*pos + 1 + c > len) return -1;
        *pos += 1u + c;
    }
    return -1;
}

int dns_parse_response(const UINT8 *p, UINT32 len, UINT16 id, UINT32 *ip) {
    if (len < 12 || be16(p) != id) return -1;
    UINT16 flags = be16(p + 2);
    if (!(flags & 0x8000)) return -1;                         // yanit degil
    int rcode = flags & 0xF;
    if (rcode == 3) return -2;                                // NXDOMAIN
    if (rcode != 0) return -1;
    UINT32 qd = be16(p + 4), an = be16(p + 6);
    if (qd > 4 || an > 64) return -1;
    UINT32 pos = 12;
    for (UINT32 i = 0; i < qd; i++) {
        if (skip_name(p, len, &pos) != 0 || pos + 4 > len) return -1;
        pos += 4;
    }
    for (UINT32 i = 0; i < an; i++) {
        if (skip_name(p, len, &pos) != 0 || pos + 10 > len) return -1;
        UINT16 type = be16(p + pos), cls = be16(p + pos + 2), rdl = be16(p + pos + 8);
        pos += 10;
        if (pos + rdl > len) return -1;
        if (type == 1 && cls == 1 && rdl == 4) { memcpy(ip, p + pos, 4); return 0; }   // A kaydi (CNAME'ler atlanir)
        pos += rdl;
    }
    return -3;
}

int dns_resolve_via(const char *name, UINT32 *ip, UINT32 timeout_ms, UINT32 server, UINT16 port) {
    if (!name || !ip) return -1;
    if (net_parse_ip(name, ip) == 0) return 0;                // zaten "A.B.C.D"
    if (!net_present() || !server) return -1;
    int s = udp_open(0);
    if (s < 0) return -1;
    UINT16 id = (UINT16)rand64();
    UINT8 q[300], r[600];
    int ql = dns_build_query(id, name, q, sizeof q);
    if (ql < 0) { udp_close(s); return -1; }
    int result = -1;
    UINT32 per = timeout_ms / 3; if (per < 800) per = 800;
    for (int attempt = 0; attempt < 3 && result == -1; attempt++) {
        if (udp_sendto(s, server, port, q, (UINT32)ql) != 0) continue;
        UINT64 start = timer_get_ticks();
        while (timer_get_ticks() - start < (UINT64)per * TIMER_HZ / 1000) {
            UINT32 from; UINT16 fp;
            int n = udp_recvfrom(s, r, sizeof r, &from, &fp, 50);
            if (n < 0 || from != server || fp != port) continue;
            int rc = dns_parse_response(r, (UINT32)n, id, ip);
            if (rc == 0) { result = 0; break; }
            if (rc == -2 || rc == -3) { result = -2; break; }   // ad yok / A kaydi yok: yeniden denemenin anlami yok
        }
    }
    udp_close(s);
    return result == 0 ? 0 : -1;
}

int dns_resolve(const char *name, UINT32 *ip, UINT32 timeout_ms) {
    return dns_resolve_via(name, ip, timeout_ms, net_dns(), 53);
}

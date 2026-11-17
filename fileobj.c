// DEOS -- pipe ve soket dosya nesneleri (file.c bunlari kind'a gore cagirir).
//
// Pipe: 4 KB halka tampon; okuma, veri gelene ya da TUM yazma uclari kapanana (EOF) kadar bloklar; yazma, yer acilana
// kadar bloklar, okuyucu kalmadiysa EPIPE doner. Uclar file_t nesneleridir: spawn ile paylasilirlar, uc yalnizca son
// referans kapaninca "kapanmis" sayilir (kabuk, cocuklara verdigi uclarin KENDI kopyalarini kapatmalidir).
#include "types.h"
#include "file.h"
#include "heap.h"
#include "string.h"
#include "thread.h"
#include "timer.h"
#include "net.h"
#include "config.h"
#include "cpu.h"

#define PIPE_SZ 4096

typedef struct { UINT8 buf[PIPE_SZ]; UINT32 head, len; int readers, writers; } pipe_t;

file_t *file_pipe_pair(file_t **wr) {
    pipe_t *p = kcalloc(1, sizeof *p);
    file_t *r = kcalloc(1, sizeof *r), *w = kcalloc(1, sizeof *w);
    if (!p || !r || !w) { kfree(p); kfree(r); kfree(w); return 0; }
    r->refs = 1; r->kind = FK_PIPE_R; r->obj = p;
    w->refs = 1; w->kind = FK_PIPE_W; w->obj = p;
    p->readers = p->writers = 1;
    *wr = w;
    return r;
}

// file_unref son referansta cagirir (nesne henuz serbest birakilmadan once)
void fileobj_close(file_t *f);

static long long pipe_read(file_t *f, void *buf, UINT64 n) {
    pipe_t *p = f->obj;
    UINT8 *out = buf;
    for (;;) {
        UINT64 fl = irq_save();
        if (p->len > 0) {
            UINT32 k = (UINT32)(p->len < n ? p->len : n);
            for (UINT32 i = 0; i < k; i++) out[i] = p->buf[(p->head + i) % PIPE_SZ];
            p->head = (p->head + k) % PIPE_SZ; p->len -= k;
            irq_restore(fl);
            return (long long)k;
        }
        int eof = p->writers == 0;
        irq_restore(fl);
        if (eof) return 0;
        thread_sleep_ms(1);
    }
}

static long long pipe_write(file_t *f, const void *buf, UINT64 n) {
    pipe_t *p = f->obj;
    const UINT8 *in = buf;
    UINT64 done = 0;
    while (done < n) {
        UINT64 fl = irq_save();
        if (p->readers == 0) { irq_restore(fl); return done ? (long long)done : EPIPE_RET; }
        UINT32 space = PIPE_SZ - p->len;
        if (space > 0) {
            UINT32 k = (UINT32)(n - done < space ? n - done : space);
            for (UINT32 i = 0; i < k; i++) p->buf[(p->head + p->len + i) % PIPE_SZ] = in[done + i];
            p->len += k; done += k;
            irq_restore(fl);
            continue;
        }
        irq_restore(fl);
        thread_sleep_ms(1);
    }
    return (long long)done;
}

// ------------------------------------------------------------------ soketler
typedef struct {
    int type, id;                 // id: udp soket no ya da tcp eli; -1 = henuz yok
    int bound, connected;
    UINT32 rcv_ms, snd_ms;
    UINT32 peer_ip; UINT16 peer_port;
} sock_t;

file_t *file_socket(int type, int *err) {
    *err = 0;
    if (type != SOCK_STREAM && type != SOCK_DGRAM) { *err = EINVAL_RET; return 0; }
    if (!net_present()) { *err = ENETUNREACH_RET; return 0; }
    file_t *f = kcalloc(1, sizeof *f);
    sock_t *s = kcalloc(1, sizeof *s);
    if (!f || !s) { kfree(f); kfree(s); *err = ENOSPC_RET; return 0; }
    s->type = type; s->id = -1; s->rcv_ms = 5000; s->snd_ms = 5000;
    f->refs = 1; f->kind = FK_SOCK; f->obj = s;
    return f;
}

static int tcp_err(int e) {
    switch (e) {
    case TCP_ERR_TIMEOUT: return ETIMEDOUT_RET;
    case TCP_ERR_RESET:   return ECONNRESET_RET;
    case TCP_ERR_ARG:     return EINVAL_RET;
    case TCP_ERR_NOMEM:   return ENOSPC_RET;
    case TCP_ERR_CLOSED:  return ENOTCONN_RET;
    default: return EIO_RET;
    }
}

int file_sock_is_stream(file_t *f) { return ((sock_t *)f->obj)->type == SOCK_STREAM; }

int file_sock_bind(file_t *f, unsigned port) {
    sock_t *s = f->obj;
    if (s->type != SOCK_DGRAM || s->id >= 0) return EINVAL_RET;
    int id = udp_open((UINT16)port);
    if (id < 0) return EADDRINUSE_RET;
    s->id = id; s->bound = 1;
    return 0;
}

int file_sock_connect(file_t *f, unsigned ip, unsigned port) {
    sock_t *s = f->obj;
    if (!port || port > 65535) return EINVAL_RET;
    if (s->type == SOCK_STREAM) {
        if (s->id >= 0) return EINVAL_RET;
        int h = tcp_connect(ip, (UINT16)port, 10000);
        if (h < 0) return h == TCP_ERR_RESET ? ECONNREFUSED_RET : tcp_err(h);
        s->id = h; s->connected = 1; s->peer_ip = ip; s->peer_port = (UINT16)port;
        return 0;
    }
    if (s->id < 0) { int id = udp_open(0); if (id < 0) return EADDRINUSE_RET; s->id = id; }
    udp_connect(s->id, ip, (UINT16)port);
    s->connected = 1; s->peer_ip = ip; s->peer_port = (UINT16)port;
    return 0;
}

long long file_sock_send(file_t *f, const void *buf, UINT64 n, const usockaddr_t *to) {
    sock_t *s = f->obj;
    if (s->type == SOCK_STREAM) {
        if (!s->connected) return ENOTCONN_RET;
        int w = tcp_write(s->id, buf, (UINT32)n, s->snd_ms);
        return w >= 0 ? w : tcp_err(w);
    }
    if (n > UDP_MAX_PAYLOAD) return EINVAL_RET;
    if (s->id < 0) { int id = udp_open(0); if (id < 0) return EADDRINUSE_RET; s->id = id; }
    int r;
    if (to) r = udp_sendto(s->id, to->ip, to->port, buf, (UINT32)n);
    else if (s->connected) r = udp_send(s->id, buf, (UINT32)n);
    else return EINVAL_RET;
    return r == 0 ? (long long)n : ENETUNREACH_RET;
}

long long file_sock_recv(file_t *f, void *buf, UINT64 n, usockaddr_t *from) {
    sock_t *s = f->obj;
    if (s->type == SOCK_STREAM) {
        if (!s->connected) return ENOTCONN_RET;
        int r = tcp_read(s->id, buf, (UINT32)n, s->rcv_ms);
        if (from) { from->ip = s->peer_ip; from->port = s->peer_port; from->pad = 0; }
        return r >= 0 ? r : tcp_err(r);
    }
    if (s->id < 0) return EINVAL_RET;                   // bind/connect/sendto yapilmamis
    UINT32 ip = 0; UINT16 port = 0;
    int r = udp_recvfrom(s->id, buf, (UINT32)n, &ip, &port, s->rcv_ms);
    if (r == -1) return ETIMEDOUT_RET;
    if (r < 0) return EIO_RET;
    if (from) { from->ip = ip; from->port = port; from->pad = 0; }
    return r;
}

int file_sock_opt(file_t *f, int opt, unsigned val) {
    sock_t *s = f->obj;
    if (opt == SO_RCVTIMEO) s->rcv_ms = val;
    else if (opt == SO_SNDTIMEO) s->snd_ms = val;
    else return EINVAL_RET;
    return 0;
}

// ------------------------------------------------------------------ ortak: kapanis + okuma/yazma
void fileobj_close(file_t *f) {
    if (f->kind == FK_PIPE_R || f->kind == FK_PIPE_W) {
        pipe_t *p = f->obj;
        UINT64 fl = irq_save();
        if (f->kind == FK_PIPE_R) p->readers--; else p->writers--;
        int dead = p->readers == 0 && p->writers == 0;
        irq_restore(fl);
        if (dead) kfree(p);
    } else if (f->kind == FK_SOCK) {
        sock_t *s = f->obj;
        if (s->id >= 0) { if (s->type == SOCK_STREAM) tcp_close(s->id); else udp_close(s->id); }
        kfree(s);
    }
}

long long fileobj_read(file_t *f, void *buf, UINT64 n) {
    if (f->kind == FK_PIPE_R) return pipe_read(f, buf, n);
    if (f->kind == FK_SOCK)   return file_sock_recv(f, buf, n, 0);
    return EBADF_RET;
}
long long fileobj_write(file_t *f, const void *buf, UINT64 n) {
    if (f->kind == FK_PIPE_W) return pipe_write(f, buf, n);
    if (f->kind == FK_SOCK)   return file_sock_send(f, buf, n, 0);
    return EBADF_RET;
}

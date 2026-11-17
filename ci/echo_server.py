#!/usr/bin/env python3
"""DEOS CI: host tarafinda UDP + TCP yanki (echo) sunucusu (UDP tarafi DNS sorgularini da yanitlar). Guest, QEMU slirp uzerinden 10.0.2.2:<port>'a baglanir
(slirp bunu host'un 127.0.0.1'ine cevirir). Her gelen veri aynen geri gonderilir; TCP'de baglanti kapaninca sunucu
devam eder. Kullanim: echo_server.py <port>"""
import socket, sys, threading

port = int(sys.argv[1]) if len(sys.argv) > 1 else 47007

def dns_answer(q):
    """q bir DNS sorgusuysa (QR=0, flags=0x0100, 1 soru, yanit yok, gecerli ad) yaniti dondurur; degilse None."""
    if len(q) < 17 or q[2:4] != b"\x01\x00" or q[4:6] != b"\x00\x01" or q[6:12] != b"\x00" * 6:
        return None
    pos, labels = 12, []
    while pos < len(q) and q[pos]:
        n = q[pos]
        if n > 63 or pos + 1 + n > len(q):
            return None
        labels.append(q[pos + 1:pos + 1 + n].decode("ascii", "replace"))
        pos += 1 + n
    if pos + 5 > len(q) or q[pos] != 0:
        return None
    question = q[12:pos + 5]
    name = ".".join(labels).lower()
    ident = q[0:2]
    if name.endswith(".invalid"):
        return ident + b"\x81\x83" + b"\x00\x01\x00\x00\x00\x00\x00\x00" + question          # NXDOMAIN
    a_rr = b"\xc0\x0c\x00\x01\x00\x01\x00\x00\x00\x3c\x00\x04" + bytes([192, 0, 2, 123])
    if name.startswith("alias."):
        cname = b"\xc0\x0c\x00\x05\x00\x01\x00\x00\x00\x3c\x00\x02\xc0\x0c"
        return ident + b"\x81\x80" + b"\x00\x01\x00\x02\x00\x00\x00\x00" + question + cname + a_rr
    return ident + b"\x81\x80" + b"\x00\x01\x00\x01\x00\x00\x00\x00" + question + a_rr

def udp():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("127.0.0.1", port))
    while True:
        data, addr = s.recvfrom(2048)
        reply = dns_answer(data)
        s.sendto(reply if reply is not None else data, addr)

def tcp_client(c):
    try:
        while True:
            data = c.recv(4096)
            if not data:
                break
            c.sendall(data)
    except OSError:
        pass
    finally:
        c.close()

def tcp():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("127.0.0.1", port))
    s.listen(8)
    while True:
        c, _ = s.accept()
        threading.Thread(target=tcp_client, args=(c,), daemon=True).start()

threading.Thread(target=udp, daemon=True).start()
print("echo sunucusu hazir: 127.0.0.1:%d (udp+tcp)" % port, flush=True)
tcp()

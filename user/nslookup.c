// nslookup ad...: DNS (A kaydi) sorgusu. Adres olarak "A.B.C.D" verilirse oldugu gibi yazilir.
#include "ulib.h"
int main(int argc, char **argv) {
    if (argc < 2) { fputs("kullanim: nslookup <ad>...\n", stderr); return 1; }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        u32 ip;
        if (resolve(argv[i], &ip) < 0) { printf("%s: cozulemedi\n", argv[i]); rc = 1; continue; }
        char a[16]; format_ip(ip, a);
        printf("%s -> %s\n", argv[i], a);
    }
    return rc;
}

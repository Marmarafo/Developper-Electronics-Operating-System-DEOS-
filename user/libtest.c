// libtest: kullanici C kutuphanesi, pipe, ortam degiskeni ve spawn(fdmap) otomatik testi (CI: kernel selftest calistirir).
// Cikis kodu: 0 = hepsi gecti. Her kontrol "[PASS]/[FAIL] aciklama" olarak yazilir.
#include "ulib.h"

static int g_total, g_failed;
#define TMP "/LIBTEST.TMP"

static void check(int cond, const char *name) {
    g_total++;
    if (!cond) g_failed++;
    printf("  [%s] %s\n", cond ? "PASS" : "FAIL", name);
}

static int cmp_int(const void *a, const void *b) { int x = *(const int *)a, y = *(const int *)b; return x < y ? -1 : x > y; }

static void test_printf(void) {
    puts("-- printf / snprintf --");
    char b[128];
    #define EQ(expect, ...) do { snprintf(b, sizeof b, __VA_ARGS__); if (strcmp(b, expect)) printf("    beklenen '%s', gelen '%s'\n", expect, b); check(!strcmp(b, expect), "snprintf: " expect); } while (0)
    EQ("-42", "%d", -42);
    EQ("   42|42   |", "%5d|%-5d|", 42, 42);
    EQ("00042", "%05d", 42);
    EQ("+7", "%+d", 7);
    EQ("-0007", "%05d", -7);
    EQ("ff", "%x", 255);
    EQ("0XFF", "%#X", 255);
    EQ("10", "%o", 8);
    EQ("4294967295", "%u", 4294967295u);
    EQ("-123456789012", "%ld", -123456789012L);
    EQ("18446744073709551615", "%llu", 18446744073709551615ULL);
    EQ("abc|       abc|abc       |ab", "%s|%10s|%-10s|%.2s", "abc", "abc", "abc", "abc");
    EQ("x%y", "%c%%%c", 'x', 'y');
    EQ("   42", "%*d", 5, 42);
    EQ("00042", "%.5d", 42);
    EQ("(null)", "%s", (char *)0);
    EQ("44", "%hhd", 300);
    EQ("4464", "%hd", 70000);
    EQ("123", "%zu", (size_t)123);
    EQ("0", "%d", 0);
    EQ("", "%.0d", 0);
    snprintf(b, sizeof b, "%p", (void *)b);
    check(b[0] == '0' && b[1] == 'x', "snprintf: %p '0x' ile baslar");
    char t[5];
    int n = snprintf(t, sizeof t, "%s", "abcdefgh");
    check(n == 8 && !strcmp(t, "abcd"), "snprintf: kirpma + tam uzunluk dondurur, NUL ile biter");
    check(snprintf(0, 0, "%d", 12345) == 5, "snprintf: tampon yok -> yalniz uzunluk");
    char big[2000]; memset(big, 'z', sizeof big - 1); big[sizeof big - 1] = 0;
    FILE *f = fopen(TMP, "w");
    int pr = f ? fprintf(f, "%s", big) : -1;
    if (f) fclose(f);
    check(pr == 1999, "fprintf: tampondan uzun cikti (heap uzerinden) tamam yazildi");
    remove(TMP);
}

static void test_malloc(void) {
    puts("-- malloc / free / realloc --");
    enum { N = 300 };
    static char *p[N]; static size_t sz[N];
    int ok = 1;
    for (int i = 0; i < N; i++) {
        sz[i] = (size_t)(1 + (rand() % 700));
        p[i] = malloc(sz[i]);
        if (!p[i] || ((size_t)p[i] & 15)) { ok = 0; break; }
        memset(p[i], i & 0xFF, sz[i]);
    }
    check(ok, "malloc: 300 blok, hepsi 16 bayt hizali");
    int intact = 1;
    for (int i = 0; i < N && ok; i++) for (size_t k = 0; k < sz[i]; k++) if ((u8)p[i][k] != (i & 0xFF)) { intact = 0; break; }
    check(intact, "malloc: bloklar birbirinin verisini ezmedi");
    for (int i = 0; i < N; i += 2) { free(p[i]); p[i] = 0; }
    int ok2 = 1;
    for (int i = 0; i < N; i += 2) { p[i] = malloc(100); if (!p[i]) ok2 = 0; else memset(p[i], 0x5A, 100); }
    check(ok2, "malloc: serbest birakilan bosluklar yeniden kullanildi");
    int intact2 = 1;
    for (int i = 1; i < N; i += 2) for (size_t k = 0; k < sz[i]; k++) if ((u8)p[i][k] != (i & 0xFF)) { intact2 = 0; break; }
    check(intact2, "malloc: yeniden kullanim komsu bloklari bozmadi");
    for (int i = 0; i < N; i++) free(p[i]);
    check(heap_used() == 0, "free: hepsi serbest -> heap_used() == 0");

    char *a = malloc(10); memcpy(a, "0123456789", 10);
    a = realloc(a, 5000);
    check(a && !memcmp(a, "0123456789", 10), "realloc: buyutme veriyi korur");
    a = realloc(a, 4);
    check(a && !memcmp(a, "0123", 4), "realloc: kucultme veriyi korur");
    free(a);
    check(realloc(0, 64) != 0, "realloc(NULL, n) = malloc");
    unsigned *z = calloc(1000, sizeof *z);
    int zero = z != 0;
    for (int i = 0; i < 1000 && zero; i++) if (z[i]) zero = 0;
    check(zero, "calloc: sifirlanmis bellek");
    free(z);
    check(calloc(0x7FFFFFFF, 0x7FFFFFFF) == 0, "calloc: tasma reddedilir");
    char *big = malloc(1 << 20);
    check(big != 0, "malloc: 1 MB");
    if (big) { memset(big, 1, 1 << 20); free(big); }
    i64 brk0 = sys_sbrk(0);
    for (int i = 0; i < 200; i++) { char *t = malloc(100000); if (!t) break; t[0] = 1; free(t); }
    i64 brk1 = sys_sbrk(0);
    check(brk1 - brk0 <= 262144, "malloc/free dongusu: heap sinirsiz buyumez (sbrk <= 256 KB artti)");
    check(malloc(0x7FFFFFFFFFFFULL) == 0, "malloc: absurt boyut 0 doner");
}

static void test_string(void) {
    puts("-- string / stdlib --");
    check(strstr("merhaba dunya", "dun") != 0 && !strstr("merhaba", "xyz") && strstr("abc", "") != 0, "strstr");
    char b[32] = "ab"; strncat(b, "cdefgh", 3); strcat(b, "!");
    check(!strcmp(b, "abcde!"), "strcat / strncat");
    char *d = strdup("kopya"); check(d && !strcmp(d, "kopya"), "strdup"); free(d);
    char m[16] = "0123456789"; memmove(m + 2, m, 8);
    check(!memcmp(m, "0101234567", 10), "memmove: ortusen ileri kopya");
    memmove(m, m + 2, 8);
    check(!memcmp(m, "01234567", 8), "memmove: ortusen geri kopya");
    check(strtol("  -123x", 0, 10) == -123 && strtol("0x1F", 0, 0) == 31 && strtol("777", 0, 8) == 511 && atoi("42") == 42, "strtol / atoi");
    char *end; strtol("12abc", &end, 10); check(*end == 'a', "strtol: end gostericisi");
    check(!strcasecmp("MeRhaBa", "merhaba") && strcasecmp("a", "b") < 0, "strcasecmp");
    check(strspn("aabbcc", "ab") == 4 && strcspn("hello world", " ") == 5, "strspn / strcspn");
    int arr[500];
    for (int i = 0; i < 500; i++) arr[i] = rand() % 1000;
    qsort(arr, 500, sizeof arr[0], cmp_int);
    int sorted = 1; for (int i = 1; i < 500; i++) if (arr[i - 1] > arr[i]) sorted = 0;
    check(sorted, "qsort: 500 eleman siralandi");
    int key = arr[250];
    int *hit = bsearch(&key, arr, 500, sizeof arr[0], cmp_int);
    check(hit && *hit == key, "bsearch");
    check(isdigit('7') && !isdigit('a') && isalpha('Q') && isspace('\t') && toupper('a') == 'A' && tolower('Z') == 'z' && isxdigit('f'), "ctype");
    srand(5); int r1 = rand(); srand(5); check(rand() == r1, "srand/rand: ayni tohum ayni dizi");
}

static void test_stdio(void) {
    puts("-- stdio --");
    FILE *f = fopen(TMP, "w");
    check(f != 0, "fopen(w)");
    if (!f) return;
    for (int i = 0; i < 3000; i++) fprintf(f, "satir %d: %s\n", i, "abcdefghij");
    check(ftell(f) > 0, "ftell: yazma sonrasi konum");
    check(fclose(f) == 0, "fclose: tamponlar bosaltildi");
    f = fopen(TMP, "r");
    int n = 0, content = 1; char line[100];
    while (f && fgets(line, sizeof line, f)) { char exp[64]; snprintf(exp, sizeof exp, "satir %d: abcdefghij\n", n); if (strcmp(line, exp)) content = 0; n++; }
    check(f && n == 3000 && content && feof(f), "fgets: 3000 satir birebir okundu, feof");
    if (f) {
        check(fseek(f, 0, SEEK_END) == 0 && ftell(f) > 50000, "fseek(SEEK_END) / ftell = dosya boyutu");
        rewind(f);
        check(fgetc(f) == 's' && ungetc('X', f) == 'X' && fgetc(f) == 'X' && fgetc(f) == 'a', "ungetc + fgetc");
        check(fseek(f, 6, SEEK_SET) == 0 && fgetc(f) == '0', "fseek(SEEK_SET)");
        fclose(f);
    }
    f = fopen(TMP, "r+");
    if (f) { fseek(f, 6, SEEK_SET); fputc('9', f); fflush(f); fseek(f, 0, SEEK_SET); fgets(line, sizeof line, f); fclose(f); }
    check(f && !strncmp(line, "satir 9:", 8), "r+: ortada guncelleme (okuma/yazma gecisi)");
    f = fopen(TMP, "a"); if (f) { fputs("SON\n", f); fclose(f); }
    f = fopen(TMP, "r"); long sz = 0;
    if (f) { fseek(f, -4, SEEK_END); fgets(line, sizeof line, f); fclose(f); }
    check(!strcmp(line, "SON\n"), "a: sona ekleme");
    static u8 bin[10000], back[10000];
    for (int i = 0; i < 10000; i++) bin[i] = (u8)(i * 13 + (i >> 5));
    f = fopen(TMP, "w+");
    size_t w = f ? fwrite(bin, 1, sizeof bin, f) : 0;
    if (f) rewind(f);
    size_t r = f ? fread(back, 1, sizeof back, f) : 0;
    check(w == 10000 && r == 10000 && !memcmp(bin, back, sizeof bin), "fwrite/fread: 10000 bayt ikili veri (w+ gidis-donus)");
    check(f && fread(back, 1, 1, f) == 0 && feof(f), "fread: dosya sonunda 0 + feof");
    if (f) fclose(f);
    (void)sz;
    check(fopen("/YOK/YOK.TXT", "r") == 0, "fopen: olmayan dosya NULL");
    check(remove(TMP) == 0 && fopen(TMP, "r") == 0, "remove");
    check(fopen(TMP, "z") == 0, "fopen: gecersiz kip reddedilir");
}

static void test_env(void) {
    puts("-- ortam degiskenleri --");
    char *path = getenv("PATH");
    check(path && !strcmp(path, "/bin"), "varsayilan PATH=/bin");
    check(getenv("YOK_BOYLE_DEGISKEN") == 0, "olmayan degisken NULL");
    check(setenv("FOO", "bar", 1) == 0 && !strcmp(getenv("FOO"), "bar"), "setenv / getenv");
    check(setenv("FOO", "baz", 0) == 0 && !strcmp(getenv("FOO"), "bar"), "setenv(overwrite=0) var olani korur");
    check(setenv("FOO", "yeni deger", 1) == 0 && !strcmp(getenv("FOO"), "yeni deger"), "setenv(overwrite=1) degistirir");
    check(setenv("A=B", "x", 1) != 0 && setenv("", "x", 1) != 0 && setenv("1ABC", "x", 1) != 0, "gecersiz degisken adlari reddedilir");
    check(unsetenv("FOO") == 0 && getenv("FOO") == 0, "unsetenv");
    char name[16]; int ok = 1;
    for (int i = 0; i < 40; i++) { snprintf(name, sizeof name, "V%d", i); if (setenv(name, "0123456789", 1) != 0) { ok = (i > 10); break; } }
    check(ok, "ortam dolunca setenv hata verir (cokme yok)");
    char blk[ENV_MAX_USER];
    int n = environ_block(blk, sizeof blk);
    check(n > 0 && blk[n - 1] == 0 && blk[n - 2] == 0, "environ_block: cift NUL ile biter");
    for (int i = 0; i < 40; i++) { snprintf(name, sizeof name, "V%d", i); unsetenv(name); }
    check(getenv("V0") == 0 && getenv("PATH") != 0, "temizlik: eklenenler silindi, PATH duruyor");
}

static int read_all(int fd, char *buf, int cap) {
    int total = 0; i64 n;
    while (total < cap && (n = sys_read(fd, buf + total, (u64)(cap - total))) > 0) total += (int)n;
    return total;
}

static void test_pipe(void) {
    puts("-- pipe / spawn(fdmap) / boru hatti --");
    int fds[2];
    check(pipe(fds) == 0, "pipe()");
    check(sys_write(fds[1], "merhaba", 7) == 7, "boruya yazildi");
    char b[32]; i64 n = sys_read(fds[0], b, sizeof b);
    check(n == 7 && !memcmp(b, "merhaba", 7), "borudan ayni veri okundu");
    close(fds[1]);
    check(sys_read(fds[0], b, sizeof b) == 0, "yazma ucu kapaninca okuma EOF (0) doner");
    close(fds[0]);
    check(pipe(fds) == 0, "ikinci pipe");
    close(fds[0]);
    check(sys_write(fds[1], "x", 1) == EPIPE_RET, "okuyucu yokken yazma EPIPE doner");
    close(fds[1]);

    // dosya -> cat -> pipe -> ebeveyn (20000 bayt: boru 4 KB; cocuk, ebeveyn okudukca ilerler)
    static u8 data[20000]; static char got[20001];
    for (int i = 0; i < 20000; i++) data[i] = (u8)('a' + (i * 7) % 26);
    FILE *f = fopen(TMP, "w"); if (f) { fwrite(data, 1, sizeof data, f); fclose(f); }
    pipe(fds);
    int fdmap[3] = { -1, fds[1], -1 };
    char *av[] = { "cat", TMP, 0 };
    i64 pid = spawn("cat", av, fdmap);
    close(fds[1]);
    check(pid > 0, "spawn(cat) stdout -> pipe (PATH ile bulundu)");
    int total = read_all(fds[0], got, 20001);
    int st = -9; wait((int)pid, &st);
    check(total == 20000 && !memcmp(got, data, 20000) && st == 0, "20000 bayt boru uzerinden birebir (4 KB tampondan buyuk)");
    close(fds[0]);

    // echo | cat | ebeveyn
    int p1[2], p2[2];
    pipe(p1); pipe(p2);
    int m1[3] = { -1, p1[1], -1 }, m2[3] = { p1[0], p2[1], -1 };
    char *a1[] = { "echo", "merhaba", "boru", 0 }, *a2[] = { "cat", 0 };
    i64 c1 = spawn("echo", a1, m1);
    close(p1[1]);
    i64 c2 = spawn("cat", a2, m2);
    close(p1[0]); close(p2[1]);
    memset(got, 0, sizeof got);
    int t2 = read_all(p2[0], got, 100);
    int s1 = -9, s2 = -9; wait((int)c1, &s1); wait((int)c2, &s2);
    close(p2[0]);
    check(c1 > 0 && c2 > 0 && t2 == 13 && !strcmp(got, "merhaba boru\n") && s1 == 0 && s2 == 0, "echo | cat | ebeveyn: iki asamali boru hatti");

    // ortam mirasi: ebeveynin FOO'su cocuga gecer
    setenv("FOO", "miras", 1);
    pipe(fds);
    int m3[3] = { -1, fds[1], -1 };
    char *a3[] = { "env", 0 };
    i64 c3 = spawn("env", a3, m3);
    close(fds[1]);
    memset(got, 0, sizeof got);
    read_all(fds[0], got, 600);
    wait((int)c3, &st);
    close(fds[0]);
    check(c3 > 0 && strstr(got, "FOO=miras\n") && strstr(got, "PATH=/bin\n"), "ortam degiskenleri cocuk surece miras kalir");
    unsetenv("FOO");

    // PATH degisince komut bulunamaz; geri alinca bulunur
    setenv("PATH", "/yok", 1);
    char *a4[] = { "cat", 0 };
    check(spawn("cat", a4, 0) == ENOENT_RET, "PATH=/yok iken 'cat' bulunamaz");
    setenv("PATH", "/yok:/bin", 1);
    int m5[3] = { -1, -1, -1 };
    pipe(fds); m5[1] = fds[1];
    char *a5[] = { "echo", "ok", 0 };
    i64 c5 = spawn("echo", a5, m5);
    close(fds[1]);
    memset(got, 0, sizeof got); read_all(fds[0], got, 10); wait((int)c5, &st); close(fds[0]);
    check(c5 > 0 && !strcmp(got, "ok\n"), "PATH iki dizinli (/yok:/bin): ikincisinde bulunur");
    setenv("PATH", "/bin", 1);
    remove(TMP);

    // kabuk: sh -c "..." (boru hatti, yonlendirme, degisken genisletme)
    {
        char *sh1[] = { "sh", "-c", "echo hello world | grep world | wc", 0 };
        pipe(fds); int ms[3] = { -1, fds[1], -1 };
        i64 sp = spawn("sh", sh1, ms); close(fds[1]);
        memset(got, 0, sizeof got); read_all(fds[0], got, 200); wait((int)sp, &st); close(fds[0]);
        char *e1; long l1 = strtol(got, &e1, 10), w1 = strtol(e1, &e1, 10), b1 = strtol(e1, &e1, 10);
        check(sp > 0 && l1 == 1 && w1 == 2 && b1 == 12, "sh -c 'echo | grep | wc': uc asamali boru hatti dogru sayar");

        char *sh2[] = { "sh", "-c", "echo ortam: ${USER} ve $HOME $? > /SHT.TMP", 0 };
        i64 s2 = spawn("sh", sh2, 0); wait((int)s2, &st);
        memset(got, 0, sizeof got); FILE *ff = fopen("/SHT.TMP", "r"); if (ff) { fgets(got, 100, ff); fclose(ff); }
        check(!strcmp(got, "ortam: deos ve / 0\n"), "sh: $DEGISKEN, ${AD}, $? genisletme + '>' yonlendirme");

        char *sh3[] = { "sh", "-c", "echo yeni >> /SHT.TMP", 0 };
        i64 s3 = spawn("sh", sh3, 0); wait((int)s3, &st);
        char *sh4[] = { "sh", "-c", "cat < /SHT.TMP | grep -c yeni", 0 };
        pipe(fds); int m4[3] = { -1, fds[1], -1 };
        i64 s4 = spawn("sh", sh4, m4); close(fds[1]);
        memset(got, 0, sizeof got); read_all(fds[0], got, 50); wait((int)s4, &st); close(fds[0]);
        check(!strcmp(got, "1\n"), "sh: '>>' ekleme + '<' girdi yonlendirme + grep -c");

        char *sh5[] = { "sh", "-c", "export KABUK=degeri", 0 };
        char *sh6[] = { "sh", "-c", "bu_komut_yok | cat", 0 };
        pipe(fds); int m6[3] = { -1, fds[1], fds[1] };
        i64 s6 = spawn("sh", sh6, m6); close(fds[1]);
        memset(got, 0, sizeof got); read_all(fds[0], got, 200); wait((int)s6, &st); close(fds[0]);
        check(strstr(got, "komut bulunamadi") != 0, "sh: olmayan komut boru hattinda hata mesaji verir, takilmaz");
        i64 s5 = spawn("sh", sh5, 0); wait((int)s5, &st);
        check(getenv("KABUK") == 0, "sh export yalniz kendi surecini etkiler (ebeveyn ortami degismez)");
        remove("/SHT.TMP");
    }

    // fd sizintisi: 100 pipe ac-kapa, fd tablosu tukenmemeli
    int leak_ok = 1;
    for (int i = 0; i < 100; i++) { if (pipe(fds) != 0) { leak_ok = 0; break; } close(fds[0]); close(fds[1]); }
    check(leak_ok, "100 pipe ac/kapa: fd tablosu tukenmedi");
    int pf[8][2], cnt = 0;
    for (; cnt < 8; cnt++) if (pipe(pf[cnt]) != 0) break;
    check(cnt >= 6 && cnt < 8 && pipe(fds) == EMFILE_RET, "fd tablosu dolunca pipe EMFILE doner");
    for (int i = 0; i < cnt; i++) { close(pf[i][0]); close(pf[i][1]); }
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    test_printf();
    test_malloc();
    test_string();
    test_stdio();
    test_env();
    test_pipe();
    printf("libtest: %d/%d gecti\n", g_total - g_failed, g_total);
    return g_failed ? 1 : 0;
}

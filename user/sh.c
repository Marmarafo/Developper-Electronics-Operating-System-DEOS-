// DEOS kabugu (ring 3): satir duzenleme, "tirnakli arguman", $DEGISKEN genisletme, boru hatti (a | b | c),
// yonlendirme (< dosya, > dosya, >> dosya), arka plan (&), ic komutlar: cd, pwd, exit, help, export, unset, env.
// Diger her sey PATH (varsayilan /bin) uzerinden <ad>.elf olarak spawn edilir.
#include "ulib.h"

#define MAXTOK   16
#define MAXSTAGE 8

static int read_line(char *buf, int cap) {
    int n = 0;
    for (;;) {
        char c;
        if (sys_read(0, &c, 1) <= 0) { sys_sleep(10); continue; }
        if (c == '\n') { u_puts("\n"); buf[n] = 0; return n; }
        if (c == '\b') { if (n > 0) { n--; sys_write(1, "\b", 1); } continue; }
        if (c >= 32 && c < 127 && n < cap - 1) { buf[n++] = c; sys_write(1, &c, 1); }
    }
}

// $AD, ${AD}, $? genisletir (tirnak icinde de). out'a yazar; 0 | -1 (tasti)
static int expand(const char *in, char *out, int cap, int last_status) {
    int o = 0;
    for (const char *p = in; *p; ) {
        if (*p != '$') { if (o >= cap - 1) return -1; out[o++] = *p++; continue; }
        p++;
        char name[40]; int n = 0; const char *val = 0; char num[16];
        if (*p == '?') { snprintf(num, sizeof num, "%d", last_status); val = num; p++; }
        else {
            if (*p == '{') { p++; while (*p && *p != '}' && n < 39) name[n++] = *p++; if (*p == '}') p++; }
            else while ((isalnum((u8)*p) || *p == '_') && n < 39) name[n++] = *p++;
            name[n] = 0;
            if (n == 0) { if (o >= cap - 1) return -1; out[o++] = '$'; continue; }
            val = getenv(name);
        }
        if (val) for (; *val; val++) { if (o >= cap - 1) return -1; out[o++] = *val; }
    }
    out[o] = 0;
    return 0;
}

// Satirdaki ilk tirnak-disi 'op' ('<' ya da '>') ile hedef dosya adini ayiklar ve satirdan siler.
// 1 = var, 0 = yok, -1 = dosya adi eksik. '>' icin '>>' = ekleme.
static int extract_redirect(char *line, char op, char *name, int cap, int *append) {
    int inq = 0;
    for (char *p = line; *p; p++) {
        if (*p == '"') { inq = !inq; continue; }
        if (inq || *p != op) continue;
        char *start = p;
        if (append) *append = 0;
        p++;
        if (op == '>' && *p == '>') { if (append) *append = 1; p++; }
        while (*p == ' ') p++;
        int n = 0;
        if (*p == '"') { p++; while (*p && *p != '"' && n < cap - 1) name[n++] = *p++; if (*p == '"') p++; }
        else while (*p && *p != ' ' && *p != '|' && n < cap - 1) name[n++] = *p++;
        name[n] = 0;
        for (char *q = start; q < p; q++) *q = ' ';
        return n > 0 ? 1 : -1;
    }
    return 0;
}

// Satiri tirnak-disi '|' karakterlerinden boler (yerinde); asamalarin baslangiclarini dondurur.
static int split_pipeline(char *line, char **stage, int max) {
    int n = 0, inq = 0;
    stage[n++] = line;
    for (char *p = line; *p; p++) {
        if (*p == '"') inq = !inq;
        else if (*p == '|' && !inq) { *p = 0; if (n >= max) return -1; stage[n++] = p + 1; }
    }
    return n;
}

static int tokenize(char *line, char **tok, int max) {
    int n = 0;
    char *p = line;
    while (*p && n < max) {
        while (*p == ' ') p++;
        if (!*p) break;
        if (*p == '"') { tok[n++] = ++p; while (*p && *p != '"') p++; if (*p) *p++ = 0; continue; }
        tok[n++] = p;
        while (*p && *p != ' ') p++;
        if (*p) *p++ = 0;
    }
    return n;
}

static void help(void) {
    u_puts("DEOS kabugu. Ic komutlar: cd [dizin], pwd, exit [kod], help, export AD=DEGER, unset AD, env\n");
    u_puts("Programlar: ls cat echo mkdir rm touch cp mv wc hexdump grep head env ifconfig nslookup nc  (PATH icindeki .elf dosyalari)\n");
    u_puts("Boru hatti: ls | grep elf | wc      Yonlendirme: komut > dosya, >> dosya, komut < dosya      Arka plan: komut &\n");
    u_puts("Degiskenler: echo $HOME  echo ${PATH}  echo $?   (export ile ayarlanir, cocuk programlara aktarilir)\n");
}

static int builtin(char **av, int ac, char *cwd, int *status, int *do_exit) {
    if (!strcmp(av[0], "exit")) { *do_exit = 1; *status = ac > 1 ? atoi(av[1]) : 0; return 1; }
    if (!strcmp(av[0], "help")) { help(); *status = 0; return 1; }
    if (!strcmp(av[0], "pwd"))  { printf("%s\n", cwd); fflush(0); *status = 0; return 1; }
    if (!strcmp(av[0], "cd")) {
        i64 r = chdir(ac > 1 ? av[1] : "/");
        if (r < 0) printf("cd: %s: %s\n", ac > 1 ? av[1] : "/", u_strerror(r));
        *status = r < 0 ? 1 : 0; fflush(0);
        return 1;
    }
    if (!strcmp(av[0], "export") || !strcmp(av[0], "unset") || !strcmp(av[0], "env")) {
        *status = 0;
        if (av[0][0] == 'e' && av[0][1] == 'n') {                               // env: yerlesik yazdirma
            char blk[ENV_MAX_USER];
            if (environ_block(blk, sizeof blk) > 0) for (char *p = blk; *p; p += strlen(p) + 1) puts(p);
        } else if (av[0][0] == 'u') {
            for (int i = 1; i < ac; i++) unsetenv(av[i]);
        } else {
            for (int i = 1; i < ac; i++) {
                char *eq = strchr(av[i], '=');
                if (!eq) { printf("export: AD=DEGER bekleniyor: %s\n", av[i]); *status = 1; continue; }
                *eq = 0;
                if (setenv(av[i], eq + 1, 1) < 0) { printf("export: gecersiz ad ya da ortam dolu: %s\n", av[i]); *status = 1; }
            }
        }
        fflush(0);
        return 1;
    }
    return 0;
}

// Tek bir komut satirini (boru hatti dahil) calistirir. exit yerlesigi ise *exit_code = cikis kodu + 1 olur.
static void run_line(const char *raw, char *cwd, int *last_status, int *exit_code) {
    char line[400];
        if (expand(raw, line, sizeof line, *last_status) < 0) { u_puts("sh: satir cok uzun\n"); return; }

    char *stage[MAXSTAGE];
    int ns = split_pipeline(line, stage, MAXSTAGE);
    if (ns < 0) { u_puts("sh: boru hattinda cok fazla asama\n"); return; }

    // arka plan (&): son asamanin sonunda
    int bg = 0;
    { char *e = stage[ns - 1] + strlen(stage[ns - 1]); while (e > stage[ns - 1] && e[-1] == ' ') e--; if (e > stage[ns - 1] && e[-1] == '&') { bg = 1; e[-1] = ' '; } }

    int ok = 1, in_fd = -1, prev_read = -1;
    i64 pids[MAXSTAGE]; int npid = 0;
    char *av[MAXSTAGE][MAXTOK + 1]; int ac[MAXSTAGE];
    char in_name[128], out_name[128]; int append = 0, has_in = 0, has_out = 0;

    // 1) her asamayi ayristir
    for (int s = 0; s < ns && ok; s++) {
        char nm[128]; int ap = 0;
        int ri = extract_redirect(stage[s], '<', nm, sizeof nm, 0);
        if (ri < 0) { u_puts("sh: '<' icin dosya adi gerekli\n"); ok = 0; break; }
        if (ri > 0) { if (s != 0) { u_puts("sh: '<' yalniz ilk asamada kullanilabilir\n"); ok = 0; break; } memcpy(in_name, nm, sizeof in_name); has_in = 1; }
        int ro = extract_redirect(stage[s], '>', nm, sizeof nm, &ap);
        if (ro < 0) { u_puts("sh: '>' icin dosya adi gerekli\n"); ok = 0; break; }
        if (ro > 0) { if (s != ns - 1) { u_puts("sh: '>' yalniz son asamada kullanilabilir\n"); ok = 0; break; } memcpy(out_name, nm, sizeof out_name); has_out = 1; append = ap; }
        ac[s] = tokenize(stage[s], av[s], MAXTOK);
        av[s][ac[s]] = 0;
        if (ac[s] == 0) { u_puts("sh: bos komut\n"); ok = 0; break; }
    }
    if (!ok) { (*last_status) = 2; return; }

    // 2) yerlesik komutlar yalniz tek asamada
    if (ns == 1 && !bg && !has_in && !has_out) {
        int st = 0, ex = 0;
        if (builtin(av[0], ac[0], cwd, &st, &ex)) { (*last_status) = st; if (ex) *exit_code = st + 1; return; }
    } else {
        const char *b = av[0][0];
        if (!strcmp(b, "cd") || !strcmp(b, "exit") || !strcmp(b, "export") || !strcmp(b, "unset")) { u_puts("sh: yerlesik komut boru hatti/yonlendirmede kullanilamaz\n"); (*last_status) = 2; return; }
    }

    // 3) giris/cikis dosyalari
    i64 out_fd = -1;
    if (has_in) { in_fd = open(in_name, O_RDONLY); if (in_fd < 0) { u_printf("sh: %s: %s\n", in_name, u_strerror(in_fd)); (*last_status) = 1; return; } }
    if (has_out) {
        out_fd = open(out_name, O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC));
        if (out_fd < 0) { u_printf("sh: %s: %s\n", out_name, u_strerror(out_fd)); if (in_fd >= 0) close((int)in_fd); (*last_status) = 1; return; }
    }

    // 4) asamalari baslat, aralarina boru bagla
    for (int s = 0; s < ns; s++) {
        int fdmap[3] = { -1, -1, -1 };
        int pr[2] = { -1, -1 };
        if (s == 0 && in_fd >= 0) fdmap[0] = (int)in_fd;
        if (s > 0) fdmap[0] = prev_read;
        if (s < ns - 1) {
            if (pipe(pr) < 0) { u_puts("sh: pipe olusturulamadi\n"); ok = 0; break; }
            fdmap[1] = pr[1];
        } else if (out_fd >= 0) fdmap[1] = (int)out_fd;
        i64 pid = spawn(av[s][0], av[s], fdmap);
        if (pr[1] >= 0) close(pr[1]);                                  // yazma ucu artik yalniz cocukta
        if (prev_read >= 0) { close(prev_read); prev_read = -1; }      // okuma ucu yalniz cocukta
        if (pid < 0) {
            u_printf("sh: %s: %s\n", av[s][0], pid == ENOENT_RET ? "komut bulunamadi" : u_strerror(pid));
            if (pr[0] >= 0) close(pr[0]);
            ok = 0; break;
        }
        pids[npid++] = pid;
        prev_read = pr[0];
    }
    if (prev_read >= 0) close(prev_read);
    if (in_fd >= 0) close((int)in_fd);
    if (out_fd >= 0) close((int)out_fd);

    if (bg && ok) { u_printf("[arka plan] pid %lld\n", pids[npid - 1]); (*last_status) = 0; return; }
    int status = 0;
    for (int k = 0; k < npid; k++) { int st = 0; wait((int)pids[k], &st); if (k == npid - 1) status = st; }
    if (!ok) { (*last_status) = 127; return; }
    (*last_status) = status;
    if (status == -1) u_puts("[hata nedeniyle sonlandirildi]\n");
    else if (status != 0) u_printf("[cikis kodu %d]\n", status);
}

int main(int argc, char **argv) {
    char raw[200], cwd[128];
    int last_status = 0, exit_code = 0;
    if (getcwd(cwd, sizeof cwd) < 0) { cwd[0] = '?'; cwd[1] = 0; }
    if (argc >= 3 && !strcmp(argv[1], "-c")) {                      // sh -c "komut": tek satir calistir ve cik (testler/betikler)
        run_line(argv[2], cwd, &last_status, &exit_code);
        fflush(0);
        return exit_code ? exit_code - 1 : last_status;
    }
    u_puts("DEOS kabugu (ring 3). 'help' yazin; 'exit' cikar.\n");
    for (;;) {
        if (getcwd(cwd, sizeof cwd) < 0) { cwd[0] = '?'; cwd[1] = 0; }
        u_printf("deos:%s$ ", cwd);
        if (read_line(raw, sizeof raw) == 0) continue;
        run_line(raw, cwd, &last_status, &exit_code);
        if (exit_code) return exit_code - 1;
    }
}

// DEOS kabugu (ring 3): satir duzenleme, "tirnakli arguman", > ve >> yonlendirme, & arka plan,
// ic komutlar: cd, pwd, exit, help. Diger her sey /bin/<ad>.elf olarak spawn edilir.
#include "ulib.h"

#define MAXTOK 16

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

// Satirdaki ilk tirnak-disi '>' veya '>>' ile hedef dosya adini ayiklar ve satirdan siler.
// 1 = yonlendirme var, 0 = yok, -1 = dosya adi eksik
static int extract_redirect(char *line, char *name, int cap, int *append) {
    int inq = 0;
    for (char *p = line; *p; p++) {
        if (*p == '"') { inq = !inq; continue; }
        if (inq || *p != '>') continue;
        char *start = p;
        *append = 0;
        p++;
        if (*p == '>') { *append = 1; p++; }
        while (*p == ' ') p++;
        int n = 0;
        if (*p == '"') { p++; while (*p && *p != '"' && n < cap - 1) name[n++] = *p++; if (*p == '"') p++; }
        else while (*p && *p != ' ' && n < cap - 1) name[n++] = *p++;
        name[n] = 0;
        for (char *q = start; q < p; q++) *q = ' ';
        return n > 0 ? 1 : -1;
    }
    return 0;
}

// Satiri bosluklara/tirnaklara gore yerinde boler.
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
    u_puts("DEOS kabugu. Ic komutlar: cd [dizin], pwd, exit [kod], help\n");
    u_puts("Programlar: ls cat echo mkdir rm touch cp mv wc hexdump  (ve /bin altindaki diger .elf dosyalari)\n");
    u_puts("Yonlendirme: komut > dosya    komut >> dosya      Arka plan: komut &\n");
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    char line[200], cwd[128], redir[128];
    u_puts("DEOS kabugu (ring 3). 'help' yazin; 'exit' cikar.\n");
    for (;;) {
        if (getcwd(cwd, sizeof cwd) < 0) { cwd[0] = '?'; cwd[1] = 0; }
        u_printf("deos:%s$ ", cwd);
        if (read_line(line, sizeof line) == 0) continue;

        int append = 0;
        int rd = extract_redirect(line, redir, sizeof redir, &append);
        if (rd < 0) { u_puts("sh: yonlendirme icin dosya adi gerekli\n"); continue; }

        char *av[MAXTOK + 1];
        int ac = tokenize(line, av, MAXTOK), bg = 0;
        if (ac > 0 && !strcmp(av[ac - 1], "&")) { bg = 1; ac--; }
        av[ac] = 0;
        if (ac == 0) continue;

        if (!strcmp(av[0], "exit")) { int code = 0; if (ac > 1) for (const char *p = av[1]; *p >= '0' && *p <= '9'; p++) code = code * 10 + (*p - '0'); return code; }
        if (!strcmp(av[0], "help")) { help(); continue; }
        if (!strcmp(av[0], "pwd"))  { u_printf("%s\n", cwd); continue; }
        if (!strcmp(av[0], "cd")) {
            i64 r = chdir(ac > 1 ? av[1] : "/");
            if (r < 0) u_printf("cd: %s: %s\n", ac > 1 ? av[1] : "/", u_strerror(r));
            continue;
        }

        int fdmap[3] = { -1, -1, -1 };
        i64 ofd = -1;
        if (rd > 0) {
            ofd = open(redir, O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC));
            if (ofd < 0) { u_printf("sh: %s: %s\n", redir, u_strerror(ofd)); continue; }
            fdmap[1] = (int)ofd;
        }
        i64 pid = spawn(av[0], av, fdmap);
        if (ofd >= 0) close((int)ofd);
        if (pid < 0) { u_printf("sh: %s: %s\n", av[0], pid == ENOENT_RET ? "komut bulunamadi" : u_strerror(pid)); continue; }
        if (bg) { u_printf("[arka plan] pid %lld\n", pid); continue; }
        int status = 0;
        wait((int)pid, &status);
        if (status == -1) u_puts("[hata nedeniyle sonlandirildi]\n");
        else if (status != 0) u_printf("[cikis kodu %d]\n", status);
    }
}

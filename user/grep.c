// grep [-v] [-c] [-n] [-i] DESEN [dosya...]: satirlarda DESEN dizgesini arar (duzenli ifade yok). Dosya yoksa stdin.
#include "ulib.h"
static int g_v, g_c, g_n, g_i;
static int matches(const char *line, const char *pat) {
    if (!g_i) return strstr(line, pat) != 0;
    size_t pl = strlen(pat);
    for (const char *p = line; *p; p++) {
        size_t k = 0;
        while (k < pl && p[k] && tolower((u8)p[k]) == tolower((u8)pat[k])) k++;
        if (k == pl) return 1;
    }
    return pl == 0;
}
static int run(FILE *f, const char *pat, const char *name, int multi) {
    char line[1024]; long ln = 0, count = 0; int any = 0;
    while (fgets(line, sizeof line, f)) {
        ln++;
        size_t l = strlen(line);
        int had_nl = l && line[l - 1] == '\n';
        if (had_nl) line[l - 1] = 0;
        if (matches(line, pat) == !g_v) {
            any = 1; count++;
            if (!g_c) {
                if (multi) printf("%s:", name);
                if (g_n) printf("%ld:", ln);
                printf("%s\n", line);
            }
        }
    }
    if (g_c) { if (multi) printf("%s:", name); printf("%ld\n", count); }
    return any;
}
int main(int argc, char **argv) {
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++)
        for (const char *o = argv[i] + 1; *o; o++) { if (*o == 'v') g_v = 1; else if (*o == 'c') g_c = 1; else if (*o == 'n') g_n = 1; else if (*o == 'i') g_i = 1; }
    if (i >= argc) { fputs("kullanim: grep [-v] [-c] [-n] [-i] desen [dosya...]\n", stderr); return 2; }
    const char *pat = argv[i++];
    int any = 0;
    if (i >= argc) return run(stdin, pat, "", 0) ? 0 : 1;
    for (int multi = argc - i > 1; i < argc; i++) {
        FILE *f = fopen(argv[i], "r");
        if (!f) { fprintf(stderr, "grep: %s: acilamadi\n", argv[i]); continue; }
        if (run(f, pat, argv[i], multi)) any = 1;
        fclose(f);
    }
    return any ? 0 : 1;
}

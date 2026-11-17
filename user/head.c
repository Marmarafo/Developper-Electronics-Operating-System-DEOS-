// head [-n N] [dosya]: ilk N satiri (varsayilan 10) yazar. Dosya yoksa stdin.
#include "ulib.h"
int main(int argc, char **argv) {
    int n = 10, i = 1;
    if (i + 1 < argc && !strcmp(argv[i], "-n")) { n = atoi(argv[i + 1]); i += 2; }
    FILE *f = i < argc ? fopen(argv[i], "r") : stdin;
    if (!f) { fprintf(stderr, "head: %s: acilamadi\n", argv[i]); return 1; }
    char line[1024];
    while (n > 0 && fgets(line, sizeof line, f)) { fputs(line, stdout); if (strchr(line, '\n')) n--; }
    return 0;
}

#ifndef DEOS_PROGS_H
#define DEOS_PROGS_H

// Kernel'e gomulu kullanici programi (ELF imaji). Tablo build.sh tarafindan uretilir.
typedef struct {
    const char *name;
    const unsigned char *start;
    const unsigned char *end;
} embedded_prog_t;

extern const embedded_prog_t g_progs[];   // son girdi: {0,0,0}

#endif

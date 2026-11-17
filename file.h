#ifndef DEOS_FILE_H
#define DEOS_FILE_H
#include "types.h"
#include "syscall_abi.h"

// Acik dosya nesnesi (fd tablosundan referans sayaciyla paylasilir). Konsol (stdin/stdout) da bir dosyadir.
enum { FK_CON_IN, FK_CON_OUT, FK_FILE, FK_DIR, FK_PIPE_R, FK_PIPE_W, FK_SOCK };

typedef struct file {
    int    refs;
    int    kind;
    int    flags;
    UINT64 pos;
    UINT32 dir_idx;
    char   path[160];
    void  *obj;                      // FK_PIPE_*: pipe_t*, FK_SOCK: sock_t*
} file_t;

file_t *file_pipe_pair(file_t **wr);                               // pipe: okuma ucunu doner, yazma ucunu *wr'ye koyar; NULL = bellek yok
file_t *file_socket(int type, int *err);                           // soket nesnesi
int       file_sock_is_stream(file_t *f);
int       file_sock_bind(file_t *f, unsigned port);
int       file_sock_connect(file_t *f, unsigned ip, unsigned port);
long long file_sock_send(file_t *f, const void *buf, UINT64 n, const usockaddr_t *to);
long long file_sock_recv(file_t *f, void *buf, UINT64 n, usockaddr_t *from);
int       file_sock_opt(file_t *f, int opt, unsigned val);

file_t *file_console(int fd);                                      // 0: stdin, 1/2: stdout/stderr (refs artar)
file_t *file_open(const char *abs_path, int flags, int *err);      // NULL + *err (negatif hata)
void    file_ref(file_t *f);
void    file_unref(file_t *f);

long long file_read(file_t *f, void *kbuf, UINT64 n);              // konsol girisi en az 1 karakter gelene dek bloklar
long long file_write(file_t *f, const void *kbuf, UINT64 n);
long long file_seek(file_t *f, long long off, int whence);
int       file_readdir(file_t *f, udirent_t *out);                 // 1 = girdi, 0 = son, <0 hata

int fs_err_to_errno(int fs_err);                                   // fs.c hata kodu -> syscall hata kodu

#endif

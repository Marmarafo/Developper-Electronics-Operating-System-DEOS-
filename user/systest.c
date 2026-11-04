// Ring 3'ten dosya/surec syscall'larinin otomatik testi. Cikis kodu = basarisiz test sayisi.
#include "ulib.h"

static int fails, checks;
static void check(int ok, const char *name) { checks++; if (!ok) fails++; u_printf("  [%s] %s\n", ok ? "PASS" : "FAIL", name); }

static void join(char *out, const char *a, const char *b) {
    u64 n = 0;
    while (*a) out[n++] = *a++;
    if (n && out[n - 1] != '/') out[n++] = '/';
    while (*b) out[n++] = *b++;
    out[n] = 0;
}

static i64 put(const char *path, const char *text, int flags) {
    i64 fd = open(path, flags);
    if (fd < 0) return fd;
    i64 w = sys_write((int)fd, text, strlen(text));
    close((int)fd);
    return w;
}

static int file_is(const char *path, const char *want) {
    char buf[128];
    i64 fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    i64 n = sys_read((int)fd, buf, sizeof buf - 1);
    close((int)fd);
    if (n < 0) return 0;
    buf[n] = 0;
    return !strcmp(buf, want);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    ustat_t st;
    char base[64], a[96], b[96], c[96], d[96], rel[96], out[96], cwd[128];
    join(base, stat("/mnt/sd1", &st) == 0 ? "/mnt/sd1" : "/", "_systest");
    join(a, base, "a.txt"); join(b, base, "b.txt"); join(c, base, "c.txt"); join(d, base, "d.txt");
    join(rel, base, "rel.txt"); join(out, base, "out.txt");
    unlink(a); unlink(b); unlink(c); unlink(d); unlink(rel); unlink(out); unlink(base);     // onceki calismadan kalanlar

    u_printf("systest (pid %lld): test dizini %s\n", sys_getpid(), base);

    check(getcwd(cwd, sizeof cwd) > 0, "getcwd");
    check(mkdir(base) == 0, "mkdir");
    check(mkdir(base) == EEXIST_RET, "mkdir: ayni dizin -> EEXIST");

    i64 fd = open(a, O_WRONLY | O_CREAT);
    check(fd >= 3, "open(O_CREAT): fd >= 3 (0-2 konsol)");
    check(sys_write((int)fd, "hello world", 11) == 11, "write 11 bayt");
    check(close((int)fd) == 0, "close");
    check(close((int)fd) == EBADF_RET, "close: kapali fd -> EBADF");

    check(stat(a, &st) == 0 && st.size == 11 && !st.is_dir, "stat: dosya, 11 bayt");
    check(stat(base, &st) == 0 && st.is_dir, "stat: dizin");
    check(stat("/olmayan/yol", &st) == ENOENT_RET, "stat: yok -> ENOENT");

    char buf[32];
    fd = open(a, O_RDONLY);
    check(sys_read((int)fd, buf, 5) == 5 && !memcmp(buf, "hello", 5), "read 5 bayt");
    check(lseek((int)fd, 6, SEEK_SET) == 6, "lseek(SEEK_SET)");
    check(sys_read((int)fd, buf, 5) == 5 && !memcmp(buf, "world", 5), "read: ofsetten sonra");
    check(sys_read((int)fd, buf, 5) == 0, "read: dosya sonu -> 0");
    check(lseek((int)fd, 0, SEEK_END) == 11, "lseek(SEEK_END)");
    check(lseek((int)fd, -4, SEEK_CUR) == 7, "lseek(SEEK_CUR, -4)");
    check(sys_read((int)fd, buf, 4) == 4 && !memcmp(buf, "orld", 4), "read: son 4 bayt");
    check(sys_write((int)fd, "x", 1) == EBADF_RET, "write: salt okunur fd -> EBADF");
    close((int)fd);

    check(put(a, " more", O_WRONLY | O_APPEND) == 5 && stat(a, &st) == 0 && st.size == 16, "O_APPEND: dosya 16 bayt");
    check(file_is(a, "hello world more"), "O_APPEND: icerik dogru");
    fd = open(a, O_WRONLY | O_TRUNC);
    close((int)fd);
    check(stat(a, &st) == 0 && st.size == 0, "O_TRUNC: dosya sifirlandi");

    put(b, "b", O_WRONLY | O_CREAT); put(c, "c", O_WRONLY | O_CREAT);
    i64 dfd = open(base, O_RDONLY | O_DIRECTORY);
    int seen = 0, names = 0;
    udirent_t de;
    while (dfd >= 0 && readdir((int)dfd, &de) == 1) {
        seen++;
        if (!strcmp(de.name, "a.txt") || !strcmp(de.name, "b.txt") || !strcmp(de.name, "c.txt")) names++;
    }
    check(dfd >= 0 && seen == 3 && names == 3, "readdir: 3 dosya listelendi");
    check(readdir((int)dfd, &de) == 0, "readdir: son sonrasi -> 0");
    close((int)dfd);
    check(open(base, O_RDONLY) == EISDIR_RET, "open(dizin) -> EISDIR");
    check(open(b, O_RDONLY | O_DIRECTORY) == ENOTDIR_RET, "open(dosya, O_DIRECTORY) -> ENOTDIR");

    check(rename(a, d) == 0 && stat(a, &st) == ENOENT_RET && stat(d, &st) == 0, "rename");
    check(unlink(d) == 0 && stat(d, &st) == ENOENT_RET, "unlink");
    check(unlink(d) == ENOENT_RET, "unlink: yok -> ENOENT");
    check(unlink(base) == ENOTEMPTY_RET, "unlink: dolu dizin -> ENOTEMPTY");

    check(chdir(base) == 0 && getcwd(cwd, sizeof cwd) > 0 && !strcmp(cwd, base), "chdir + getcwd");
    check(put("rel.txt", "r", O_WRONLY | O_CREAT) == 1 && stat(rel, &st) == 0, "goreli yol (cwd'ye gore)");
    check(chdir("/") == 0, "chdir(/)");
    check(chdir("/olmayan") == ENOENT_RET, "chdir: yok -> ENOENT");
    check(chdir(b) == ENOTDIR_RET, "chdir: dosya -> ENOTDIR");

    // spawn + argumanlar + yonlendirme + wait
    i64 o = open(out, O_WRONLY | O_CREAT | O_TRUNC);
    int fdmap[3] = { -1, (int)o, -1 };
    char *av[] = { "echo", "uno", "dos", 0 };
    i64 pid = spawn("echo", av, fdmap);
    check(pid > 0, "spawn(echo uno dos)");
    int status = 99;
    check(wait((int)pid, &status) == pid && status == 0, "wait: alt surec 0 ile cikti");
    close((int)o);
    check(file_is(out, "uno dos\n"), "stdout yonlendirme: argumanlar dosyaya yazildi");
    check(spawn("olmayan_program_xyz", av, 0) == ENOENT_RET, "spawn: yok -> ENOENT");
    check(wait(999999, &status) == ECHILD_RET, "wait: bilinmeyen pid -> ECHILD");

    // hatali gostericiler
    check(open((const char*)1, O_RDONLY) == EFAULT_RET, "open(gecersiz isaretci) -> EFAULT");
    fd = open(b, O_RDONLY);
    check(sys_read((int)fd, (void*)0xFFFF800000000000ULL, 8) == EFAULT_RET, "read(kernel adresi) -> EFAULT");
    check(sys_write((int)fd, (void*)0xFFFF800000000000ULL, 8) < 0, "write(kernel adresi) reddedildi");
    close((int)fd);

    // fd tablosu doluluk
    i64 fds[20];
    int opened = 0;
    i64 lasterr = 0;
    for (int i = 0; i < 20; i++) { fds[i] = open(b, O_RDONLY); if (fds[i] < 0) { lasterr = fds[i]; break; } opened++; }
    check(opened >= 10 && opened <= 13 && lasterr == EMFILE_RET, "fd tablosu dolunca -> EMFILE");
    for (int i = 0; i < opened; i++) close((int)fds[i]);

    // temizlik
    unlink(b); unlink(c); unlink(rel); unlink(out);
    check(unlink(base) == 0 && stat(base, &st) == ENOENT_RET, "temizlik: test dizini silindi");

    u_printf("systest: %d kontrol, %d basarisiz\n", checks, fails);
    return fails;
}

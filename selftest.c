// DEOS -- bellek alt sistemi (PMM + heap) otomatik testi. Shell'de 'memtest'.
#include "types.h"
#include "selftest.h"
#include "pmm.h"
#include "heap.h"
#include "kprintf.h"
#include "string.h"
#include "console.h"
#include "paging.h"
#include "thread.h"
#include "process.h"
#include "blk.h"
#include "fs.h"
#include "usb.h"

#define C_OK   0x009ECE6Au
#define C_BAD  0x00F7768Eu
#define C_FG   0x00C0CAF5u
#define C_BG   0x001A1B26u

static int g_ok;

static void check(int cond, const char *name) {
    console_set_color(cond ? C_OK : C_BAD, C_BG);
    kprintf(cond ? "  [PASS] " : "  [FAIL] ");
    console_set_color(C_FG, C_BG);
    kprintf("%s\n", name);
    if (!cond) g_ok = 0;
}

#define NBLK 64

int selftest_memory(void) {
    g_ok = 1;
    pmm_stats_t s0, s1;
    pmm_get_stats(&s0);

    // ---- PMM ----
    UINT64 a = pmm_alloc_page(), b = pmm_alloc_page();
    check(a && b && a != b && !(a & 0xFFF) && !(b & 0xFFF), "pmm: iki ayri, 4KB hizali sayfa");
    memset((void*)(UINTN)a, 0xAA, PAGE_SIZE);
    memset((void*)(UINTN)b, 0x55, PAGE_SIZE);
    check(((UINT8*)(UINTN)a)[PAGE_SIZE - 1] == 0xAA && ((UINT8*)(UINTN)b)[0] == 0x55, "pmm: sayfalar yazilabilir/okunabilir");
    check(!pmm_page_is_free(a) && !pmm_page_is_free(b), "pmm: ayrilan sayfalar 'dolu' gorunuyor");
    pmm_free_page(a); pmm_free_page(b);
    check(pmm_page_is_free(a) && pmm_page_is_free(b), "pmm: serbest birakilan sayfalar 'bos' gorunuyor");
    pmm_get_stats(&s1);
    check(s1.free_pages == s0.free_pages, "pmm: bos sayfa sayaci eski haline dondu");

    UINT64 c = pmm_alloc_pages(100);
    int contiguous = c != 0;
    for (UINT64 i = 0; c && i < 100; i++) if (pmm_page_is_free(c + i * PAGE_SIZE)) contiguous = 0;
    check(contiguous, "pmm: 100 bitisik sayfa ayrildi");
    if (c) {
        memset((void*)(UINTN)c, 0x5A, 100 * PAGE_SIZE);
        pmm_free_pages(c, 100);
    }
    pmm_get_stats(&s1);
    check(s1.free_pages == s0.free_pages, "pmm: 100 sayfa iade edildi, sayac dogru");
    check(pmm_alloc_pages(1ULL << 40) == 0, "pmm: asiri buyuk istek 0 donduruyor");

    // ---- Heap ----
    heap_stats_t h0, h1;
    heap_get_stats(&h0);
    check(h0.free_blocks >= 1 && h0.free_bytes > 0, "heap: baslangicta bos alan var");

    UINT8 *p[NBLK];
    int alloc_ok = 1, aligned = 1;
    for (int i = 0; i < NBLK; i++) {
        size_t sz = (size_t)(i * 37 + 1);
        p[i] = kmalloc(sz);
        if (!p[i]) { alloc_ok = 0; continue; }
        if ((UINTN)p[i] & 15) aligned = 0;
        memset(p[i], i + 1, sz);
    }
    check(alloc_ok, "heap: 64 farkli boyutlu blok ayrildi");
    check(aligned, "heap: tum bloklar 16 bayt hizali");

    int intact = 1;
    for (int i = 0; i < NBLK; i++) {
        size_t sz = (size_t)(i * 37 + 1);
        for (size_t k = 0; p[i] && k < sz; k++) if (p[i][k] != (UINT8)(i + 1)) { intact = 0; break; }
    }
    check(intact, "heap: bloklarin icerigi birbirini bozmadi");

    for (int i = 1; i < NBLK; i += 2) { kfree(p[i]); p[i] = 0; }
    UINT8 *q[32];
    int realloc_ok = 1;
    for (int i = 0; i < 32; i++) {
        q[i] = kmalloc(50 + i);
        if (!q[i]) realloc_ok = 0; else memset(q[i], 0xEE, 50 + i);
    }
    check(realloc_ok, "heap: serbest birakilan yerler yeniden kullanildi");

    intact = 1;
    for (int i = 0; i < NBLK; i += 2) {
        size_t sz = (size_t)(i * 37 + 1);
        for (size_t k = 0; k < sz; k++) if (p[i][k] != (UINT8)(i + 1)) { intact = 0; break; }
    }
    check(intact, "heap: sag kalan bloklar bozulmadi");

    UINT8 *z = kcalloc(10, 100);
    int zeroed = z != 0;
    for (int i = 0; z && i < 1000; i++) if (z[i]) zeroed = 0;
    check(zeroed, "heap: kcalloc sifirlanmis bellek veriyor");
    UINT8 *r = kmalloc(16);
    if (r) { memset(r, 0x77, 16); r = krealloc(r, 4000); }
    check(r && r[0] == 0x77 && r[15] == 0x77, "heap: krealloc eski veriyi koruyor");
    check(kmalloc(1ULL << 40) == 0, "heap: asiri buyuk istek NULL donduruyor");

    for (int i = 0; i < NBLK; i += 2) kfree(p[i]);
    for (int i = 0; i < 32; i++) kfree(q[i]);
    kfree(z); kfree(r);
    heap_get_stats(&h1);
    check(h1.used_bytes == h0.used_bytes && h1.blocks == h0.blocks && h1.free_bytes == h0.free_bytes,
          "heap: hepsi iade edilince bloklar birlesti, baslangic durumu geri geldi");

    kprintf(g_ok ? "Sonuc: TUM TESTLER GECTI\n" : "Sonuc: BAZI TESTLER BASARISIZ\n");
    return g_ok;
}

// ---------------------------------------------------------------- paging

extern char __text_start[], __rodata_start[], __data_start[];

int selftest_paging(BootInfo *bi) {
    g_ok = 1;
    UINT64 phys, fl;
    UINT32 sh;

    UINT64 t = (UINT64)(UINTN)__text_start;
    check(paging_query(t, &phys, &fl, &sh) && phys == t && sh == 12, ".text: birebir eslenmis, 4 KB sayfa");
    check(!(fl & PTE_W) && !(fl & PTE_NX), ".text: yazilamaz + calistirilabilir (R-X)");

    UINT64 r = (UINT64)(UINTN)__rodata_start;
    paging_query(r, &phys, &fl, &sh);
    check(!(fl & PTE_W) && (fl & PTE_NX), ".rodata: yazilamaz + calistirilamaz (R--)");

    UINT64 d = (UINT64)(UINTN)__data_start;
    paging_query(d, &phys, &fl, &sh);
    check((fl & PTE_W) && (fl & PTE_NX), ".data/.bss: yazilabilir + calistirilamaz (RW-)");

    check(!paging_query(0, &phys, &fl, &sh), "sayfa 0 eslenmemis (NULL korumasi)");
    check(paging_query(4096, &phys, &fl, &sh), "sayfa 1 eslenmis (sadece sayfa 0 bos)");

    void *hp = kmalloc(64);
    int heap_ok = hp && paging_query((UINT64)(UINTN)hp, &phys, &fl, &sh) && phys == (UINT64)(UINTN)hp
                  && (fl & PTE_W) && (fl & PTE_NX);
    check(heap_ok, "heap: RW + calistirilamaz (NX)");
    kfree(hp);

    int fb_ok = paging_query(bi->framebuffer_base, &phys, &fl, &sh) && (fl & PTE_PWT) && !(fl & PTE_PCD);
    check(fb_ok, "framebuffer: write-combining (PAT)");

    // yuksek yari: 0xFFFF8000_00200000'e yeni bir sayfa esle, yaz/oku, kaldir
    const UINT64 V = 0xFFFF800000200000ULL;
    UINT64 pg = pmm_alloc_page();
    int map_ok = pg && paging_map_4k(V, pg, PTE_W | PTE_NX) == 0;
    check(map_ok, "map: yuksek yari sanal adrese sayfa eslendi");
    if (map_ok) {
        *(volatile UINT64*)(UINTN)V = 0xDEADBEEF12345678ULL;
        check(*(volatile UINT64*)(UINTN)pg == 0xDEADBEEF12345678ULL, "map: sanal adrese yazilan veri fiziksel sayfada");
        check(paging_query(V, &phys, &fl, &sh) && phys == pg, "map: sorgu dogru fiziksel adresi veriyor");
        check(paging_map_4k(V, pg, PTE_W) == -1, "map: ayni adrese ikinci esleme reddedildi");
        check(paging_unmap_4k(V) == 0 && !paging_query(V, &phys, &fl, &sh), "unmap: esleme kaldirildi");
    }
    if (pg) pmm_free_page(pg);
    check(paging_map_4k(0x400000ULL + 0x100, 0, 0) == -1, "map: hizasiz adres reddedildi");

    kprintf(g_ok ? "Sonuc: TUM TESTLER GECTI\n" : "Sonuc: BAZI TESTLER BASARISIZ\n");
    return g_ok;
}

// ---------------------------------------------------------------- thread'ler

static volatile UINT64 g_cnt[3];
static volatile int    g_stop;

// Hic yield/sleep yapmayan, sadece sayan thread: sayaci artiyorsa PREEMPTION calisiyor demektir.
static void busy_worker(void *arg) {
    int i = (int)(UINTN)arg;
    while (!g_stop) g_cnt[i]++;
}

static void sleeper(void *arg) {
    (void)arg;
    for (int k = 0; k < 5; k++) { g_cnt[2]++; thread_sleep_ms(20); }
}

int selftest_threads(void) {
    g_ok = 1;
    g_cnt[0] = g_cnt[1] = g_cnt[2] = 0;
    g_stop = 0;

    heap_stats_t h0, h1;
    pmm_stats_t p0, p1;
    heap_get_stats(&h0);
    pmm_get_stats(&p0);
    int base_threads = thread_count();

    int a = thread_create("busy-a", busy_worker, (void*)0);
    int b = thread_create("busy-b", busy_worker, (void*)1);
    int c = thread_create("sleeper", sleeper, 0);
    check(a > 0 && b > 0 && c > 0, "thread: 3 thread olusturuldu");
    check(thread_count() == base_threads + 3, "thread: sayac 3 artti");

    thread_sleep_ms(300);                       // shell uyur, digerleri calisir

    check(g_cnt[0] > 1000 && g_cnt[1] > 1000, "preemption: sleep/yield yapmayan iki thread de calisti");
    UINT64 lo = g_cnt[0] < g_cnt[1] ? g_cnt[0] : g_cnt[1];
    UINT64 hi = g_cnt[0] < g_cnt[1] ? g_cnt[1] : g_cnt[0];
    check(lo * 3 > hi, "round-robin: iki thread benzer CPU payi aldi");
    check(g_cnt[2] == 5, "sleep: uyuyan thread 5 turu da tamamladi");

    g_stop = 1;                                 // busy thread'ler donguden cikip bitsin
    thread_sleep_ms(50);
    thread_sleep_ms(50);
    check(thread_count() == base_threads, "thread: biten thread'ler listeden dustu");

    int d = thread_create("kurban", busy_worker, (void*)2);   // g_stop=1: hemen biter ama once kill'i deneyelim
    g_stop = 0;
    int killed = thread_kill(d);
    g_stop = 1;
    thread_sleep_ms(30);
    check(d > 0 && killed == 0, "kill: baska thread sonlandirildi");
    check(thread_kill(0) == -1, "kill: shell thread'i sonlandirilamaz");
    check(thread_kill(9999) == -1, "kill: olmayan id reddedildi");
    thread_sleep_ms(30);

    heap_get_stats(&h1);
    pmm_get_stats(&p1);
    check(thread_count() == base_threads, "thread: hepsi temizlendi");
    check(h1.used_bytes == h0.used_bytes && h1.blocks == h0.blocks, "kaynak: heap eski haline dondu (sizinti yok)");
    check(p1.free_pages == p0.free_pages, "kaynak: stack sayfalari PMM'e iade edildi");

    kprintf(g_ok ? "Sonuc: TUM TESTLER GECTI\n" : "Sonuc: BAZI TESTLER BASARISIZ\n");
    return g_ok;
}

// ---------------------------------------------------------------- kullanici modu

static int run_wait(const char *name, int *code) {
    int pid = process_start_by_name(name);
    if (pid < 0) return pid;
    while (thread_alive(pid)) thread_sleep_ms(10);
    *code = 12345;
    thread_get_exit_code(pid, code);
    return pid;
}

int selftest_user(void) {
    g_ok = 1;
    heap_stats_t h0, h1;
    pmm_stats_t p0, p1;
    heap_get_stats(&h0);
    pmm_get_stats(&p0);
    int base_threads = thread_count();
    int code;

    check(prog_find("hello") && prog_find("heaptest") && prog_find("fault"), "program tablosu: hello/heaptest/fault gomulu");
    check(process_start_by_name("olmayan_program") == -1, "bulunamayan program reddedildi");

    int bad = process_start("bozuk", (const UINT8*)"bu bir ELF degil", 16);
    check(bad < 0, "bozuk ELF imaji reddedildi (kernel cokmedi)");
    pmm_get_stats(&p1);
    check(p1.free_pages == p0.free_pages, "bozuk ELF: ayrilan bellek iade edildi");

    int pid = run_wait("hello", &code);
    check(pid > 0 && code == 0, "hello: ring 3'te calisti, cikis kodu 0");

    pid = run_wait("heaptest", &code);
    check(pid > 0 && code == 0, "heaptest: sbrk + bellek dogrulama, cikis kodu 0");

    pid = run_wait("fault", &code);
    check(pid > 0 && code == -1, "fault: kernel belligine erisen surec SONLANDIRILDI (kernel yasiyor)");

    // Izolasyon: iki heaptest AYNI sanal adreslerde, farkli desenlerle ayni anda calisir
    int a = process_start_by_name("heaptest");
    int b = process_start_by_name("heaptest");
    check(a > 0 && b > 0 && a != b, "izolasyon: iki heaptest ayni anda baslatildi");
    while (thread_alive(a) || thread_alive(b)) thread_sleep_ms(10);
    int ca = 99, cb = 99;
    thread_get_exit_code(a, &ca);
    thread_get_exit_code(b, &cb);
    check(ca == 0 && cb == 0, "izolasyon: ayni sanal adreslerde bagimsiz veri, ikisi de dogru");

    // Kill: calisan bir kullanici surecini oldur
    int t = process_start_by_name("ticker");
    thread_sleep_ms(50);
    check(t > 0 && thread_kill(t) == 0, "kill: calisan kullanici sureci sonlandirildi");
    thread_sleep_ms(100);
    check(!thread_alive(t), "kill: surec listeden dustu");
    thread_sleep_ms(50);

    check(thread_count() == base_threads, "thread sayisi eski haline dondu");
    heap_get_stats(&h1);
    pmm_get_stats(&p1);
    check(h1.used_bytes == h0.used_bytes && h1.blocks == h0.blocks, "kaynak: heap sizintisi yok");
    check(p1.free_pages == p0.free_pages, "kaynak: tum surec bellegi (kod, stack, heap, tablolar) PMM'e iade edildi");

    kprintf(g_ok ? "Sonuc: TUM TESTLER GECTI\n" : "Sonuc: BAZI TESTLER BASARISIZ\n");
    return g_ok;
}

// ---------------------------------------------------------------- depolama

extern char __kernel_start[], __kernel_end[];

int selftest_storage(const char *arg) {
    g_ok = 1;
    check(blk_count() >= 1, "en az bir blok aygiti (AHCI diski) bulundu");
    blockdev_t *d = blk_get(0);
    if (!d) { kprintf("Sonuc: disk yok\n"); return 0; }

    UINT8 *a = kmalloc(4096), *b = kmalloc(4096);
    int rd_ok = a && b && blk_read(d, 0, 1, a) == 0 && blk_read(d, 0, 1, b) == 0;
    check(rd_ok && !memcmp(a, b, 512), "sektor 0 iki kez okununca ayni veri");
    check(rd_ok && a[510] == 0x55 && a[511] == 0xAA, "sektor 0: gecerli boot imzasi (0x55AA)");
    check(blk_read(d, d->sectors, 1, a) != 0, "disk sinirinin otesi okuma reddedildi");
    check(blk_read(d, d->sectors - 1, 1, a) == 0, "son sektor okunabildi");
    check(blk_read(d, 0, 8, a ? a : 0) == 0, "coklu sektor okuma (8 sektor)");

    // Bolumsuz FAT'ta veya bolumde dosya sistemi
    check(fs_mount_count() >= 1, "en az bir FAT hacmi baglandi");
    fs_dirent_t e;
    check(fs_stat("/", &e) == 0 && e.is_dir, "kok dizin var");
    check(fs_stat("/EFI/BOOT/KERNEL.BIN", &e) == 0 && !e.is_dir, "/EFI/BOOT/KERNEL.BIN bulundu");
    UINT64 ksz = (UINT64)(UINTN)__kernel_end - (UINT64)(UINTN)__kernel_start;
    check(e.size > 0 && e.size <= ksz && e.size + 4096 > ksz, "KERNEL.BIN boyutu bellekteki kernel ile uyumlu");

    UINT8 *f = kmalloc(4096);
    long long got = f ? fs_read("/EFI/BOOT/KERNEL.BIN", 0, f, 4096) : -1;
    int same = got == 4096 && !memcmp(f, __kernel_start, 4096);
    check(same, "diskten okunan KERNEL.BIN, bellekteki kernel ile BIREBIR ayni");
    if (!same) {
        long long first = -1;
        for (long long i = 0; got > 0 && i < got; i++) if (f[i] != (UINT8)__kernel_start[i]) { first = i; break; }
        kprintf("    (okunan=%lld, ilk fark ofseti=%lld)\n", got, first);
    }
    long long got2 = f ? fs_read("/EFI/BOOT/KERNEL.BIN", 1000, f, 3000) : -1;
    check(got2 == 3000 && !memcmp(f, __kernel_start + 1000, 3000), "hizasiz ofsetli okuma dogru (ofset 1000)");
    check(fs_stat("/efi/boot/kernel.bin", &e) == 0, "yol buyuk/kucuk harf duyarsiz");
    check(fs_stat("/olmayan/dosya", &e) != 0, "olmayan yol reddedildi");

    char norm[64];
    fs_normalize("/EFI/BOOT", "../../bin/./x", norm, sizeof norm);
    check(!strcmp(norm, "/bin/x"), "yol normallestirme (../ ve ./)");

    // Isteye bagli: yazma testi (son sektoru kaydedip geri yukler)
    if (!strncmp(arg, "write ", 6)) {
        blockdev_t *w = blk_find(arg + 6);
        if (!w) { check(0, "yazma testi: aygit bulunamadi"); }
        else {
            UINT64 last = w->sectors - 1;
            UINT8 *orig = kmalloc(512), *pat = kmalloc(512), *back = kmalloc(512);
            for (int i = 0; orig && i < 512; i++) pat[i] = (UINT8)(i * 7 + 13);
            int ok = orig && blk_read(w, last, 1, orig) == 0;
            ok = ok && blk_write(w, last, 1, pat) == 0 && blk_read(w, last, 1, back) == 0 && !memcmp(pat, back, 512);
            check(ok, "yazma: son sektor yazildi ve ayni veri geri okundu");
            if (orig) { blk_write(w, last, 1, orig); }
            int rest = orig && blk_read(w, last, 1, back) == 0 && !memcmp(orig, back, 512);
            check(rest, "yazma: orijinal veri geri yuklendi");
            kfree(orig); kfree(pat); kfree(back);
        }
    }
    kfree(a); kfree(b); kfree(f);
    kprintf(g_ok ? "Sonuc: TUM TESTLER GECTI\n" : "Sonuc: BAZI TESTLER BASARISIZ\n");
    return g_ok;
}

// ------------------------------------------------------------- dosya sistemi yazma

static int content_is(const char *path, const UINT8 *want, UINT64 len) {
    UINT64 sz = 0;
    UINT8 *d = fs_read_all(path, &sz);
    int ok = d && sz == len && (len == 0 || !memcmp(d, want, (size_t)len));
    kfree(d);
    return ok;
}

static int name_in_dir(const char *dir, const char *name) {
    fs_dirent_t e;
    for (int i = 0; fs_readdir(dir, i, &e) == 0; i++) if (!strcmp(e.name, name)) return 1;
    return 0;
}

static int count_dir(const char *dir) {
    fs_dirent_t e;
    int n = 0;
    while (fs_readdir(dir, n, &e) == 0) n++;
    return n;
}

int selftest_fs(void) {
    g_ok = 1;
    fs_dirent_t e;
    const char *base = fs_stat("/mnt/sd1", &e) == 0 ? "/mnt/sd1/_deostest" : "/_deostest";
    char p[160], q[160];
    kprintf("  (test dizini: %s)\n", base);

    UINT64 tot0, free0;
    fs_space(base[1] == 'm' ? "/mnt/sd1" : "/", &tot0, &free0);

    fs_remove(base);
    check(fs_create(base, 1) == 0, "mkdir: test dizini olusturuldu");
    check(fs_stat(base, &e) == 0 && e.is_dir, "mkdir: dizin olarak gorunuyor");
    check(fs_create(base, 1) == -2, "mkdir: ayni ad ikinci kez reddedildi (-2)");
    check(fs_create("/mnt/sd1/_deostest/a:b", 0) == -1 || fs_create("/_deostest/a:b", 0) == -1, "gecersiz dosya adi (:) reddedildi");

    // kucuk dosya
    ksnprintf(p, sizeof p, "%s/kucuk.txt", base);
    check(fs_write_file(p, "merhaba dunya\n", 14) == 0, "dosya olustur + yaz");
    check(content_is(p, (const UINT8*)"merhaba dunya\n", 14), "geri okunan icerik ayni");
    check(fs_stat(p, &e) == 0 && e.size == 14 && !e.is_dir, "boyut 14 bayt");
    check(!strcmp(e.name, "kucuk.txt"), "kucuk harfli 8.3 ad korundu");

    // buyuk dosya (cok kume)
    UINT32 big = 100 * 1024 + 123;
    UINT8 *pat = kmalloc(big);
    for (UINT32 i = 0; pat && i < big; i++) pat[i] = (UINT8)(i * 31 + (i >> 8));
    ksnprintf(p, sizeof p, "%s/buyuk.bin", base);
    check(pat && fs_write_file(p, pat, big) == 0, "100 KB dosya yazildi (cok kume)");
    check(pat && content_is(p, pat, big), "100 KB dosya birebir geri okundu");

    // ekleme (append) ve ortadan uzerine yazma
    check(fs_write(p, big, "SON", 3) == 3 && fs_stat(p, &e) == 0 && e.size == big + 3, "ekleme: dosya 3 bayt uzadi");
    UINT8 *rd = kmalloc(5000);
    long long got = rd ? fs_read(p, big - 2, rd, 5) : -1;
    check(got == 5 && rd[2] == 'S' && rd[3] == 'O' && rd[4] == 'N', "ekleme: sonuna yazilan veri dogru");
    UINT8 mid[2000];
    memset(mid, 0xAB, sizeof mid);
    check(fs_write(p, 1000, mid, sizeof mid) == (long long)sizeof mid, "ortadan 2000 bayt uzerine yazildi");
    got = rd ? fs_read(p, 990, rd, 2020) : -1;
    int mid_ok = got == 2020 && rd[0] == pat[990] && rd[9] == pat[999] && rd[10] == 0xAB && rd[2009] == 0xAB && rd[2010] == pat[3000];
    check(mid_ok, "uzerine yazma: komsu baytlar bozulmadi");
    check(fs_write(p, e.size + 5, "x", 1) < 0, "bosluklu (sparse) yazma reddedildi");

    // uzun ad (LFN) ve karisik harf
    ksnprintf(q, sizeof q, "%s/Uzun Dosya Adi Testi.txt", base);
    check(fs_write_file(q, "lfn", 3) == 0, "LFN: uzun adli dosya olusturuldu");
    check(name_in_dir(base, "Uzun Dosya Adi Testi.txt"), "LFN: ad birebir (buyuk/kucuk harf) listelendi");
    ksnprintf(q, sizeof q, "%s/uzun dosya adi testi.TXT", base);
    check(fs_stat(q, &e) == 0, "LFN: arama buyuk/kucuk harf duyarsiz");
    ksnprintf(q, sizeof q, "%s/KARISIK.txt", base);
    check(fs_write_file(q, "k", 1) == 0 && name_in_dir(base, "KARISIK.txt"), "karisik harf (KARISIK.txt) korundu");

    // alt dizin, tasima, yeniden adlandirma
    ksnprintf(q, sizeof q, "%s/alt", base);
    check(fs_create(q, 1) == 0, "alt dizin olusturuldu");
    ksnprintf(p, sizeof p, "%s/kucuk.txt", base);
    ksnprintf(q, sizeof q, "%s/alt/tasindi.txt", base);
    check(fs_rename(p, q) == 0, "mv: dosya alt dizine tasindi");
    check(fs_stat(p, &e) != 0 && content_is(q, (const UINT8*)"merhaba dunya\n", 14), "mv: eski yol yok, yeni yolda ayni icerik");
    ksnprintf(p, sizeof p, "%s/alt/yeniad.txt", base);
    check(fs_rename(q, p) == 0 && name_in_dir(base, "alt") && count_dir(p) >= 0 && fs_stat(p, &e) == 0, "yeniden adlandirma");
    ksnprintf(q, sizeof q, "%s/alt2", base);
    ksnprintf(p, sizeof p, "%s/alt", base);
    check(fs_rename(p, q) == 0, "dizin yeniden adlandirildi");
    ksnprintf(p, sizeof p, "%s/alt2/yeniad.txt", base);
    check(fs_stat(p, &e) == 0, "yeniden adlandirilan dizinin icerigi erisilebilir");

    // silme
    ksnprintf(p, sizeof p, "%s/alt2", base);
    check(fs_remove(p) == -3, "rmdir: dolu dizin reddedildi (-3)");
    ksnprintf(p, sizeof p, "%s/alt2/yeniad.txt", base);
    check(fs_remove(p) == 0, "rm: dosya silindi");
    ksnprintf(p, sizeof p, "%s/alt2", base);
    check(fs_remove(p) == 0, "rmdir: bos dizin silindi");

    // dizini genisletme: cok sayida (LFN'li) dosya
    int made = 0;
    ksnprintf(q, sizeof q, "%s/cok", base);
    fs_create(q, 1);
    for (int i = 0; i < 48; i++) {
        ksnprintf(p, sizeof p, "%s/cok/dosya numara %d.txt", base, i);
        if (fs_write_file(p, "x", 1) == 0) made++;
    }
    check(made == 48, "48 uzun adli dosya olusturuldu (dizin genisledi)");
    check(count_dir(q) == 48, "dizin listesi 48 girdi veriyor");
    int removed = 0;
    for (int i = 0; i < 48; i++) {
        ksnprintf(p, sizeof p, "%s/cok/dosya numara %d.txt", base, i);
        if (fs_remove(p) == 0) removed++;
    }
    check(removed == 48 && count_dir(q) == 0, "hepsi silindi, dizin bos");

    // temizlik + alan sayimi
    const char *rm_list[] = { "cok", "buyuk.bin", "Uzun Dosya Adi Testi.txt", "KARISIK.txt", 0 };
    for (int i = 0; rm_list[i]; i++) { ksnprintf(p, sizeof p, "%s/%s", base, rm_list[i]); fs_remove(p); }
    check(fs_remove(base) == 0, "test dizini silindi");
    UINT64 tot1, free1;
    fs_space(base[1] == 'm' ? "/mnt/sd1" : "/", &tot1, &free1);
    check(free1 == free0, "bos alan baslangictaki degere dondu (kume sizintisi yok)");

    kfree(pat); kfree(rd);
    kprintf(g_ok ? "Sonuc: TUM TESTLER GECTI\n" : "Sonuc: BAZI TESTLER BASARISIZ\n");
    return g_ok;
}

// ------------------------------------------------------------------------ USB

int selftest_usb(void) {
    g_ok = 1;
    int n = usb_device_count();
    kprintf("  USB cihaz sayisi: %d\n", n);
    usbdev_t *d;
    int kbd = 0, mouse = 0, msc = 0, hid_err = 0;
    for (int i = 0; (d = usb_device(i)); i++) {
        kbd += d->kind == USB_KBD; mouse += d->kind == USB_MOUSE; msc += d->kind == USB_MSC;
        if ((d->kind == USB_KBD || d->kind == USB_MOUSE) && d->errors > 20) hid_err = 1;
    }
    check(usb_event_count() > 0, "xHCI olay halkasi calisiyor (olaylar geldi)");
    check(!hid_err, "HID cihazlari: anlamli hata orani yok");
    if (kbd)   kprintf("  klavye: %d, ", kbd);
    if (mouse) kprintf("fare: %d, ", mouse);
    kprintf("depolama: %d\n", msc);

    blockdev_t *b = blk_find("usb0");
    if (!b) { kprintf("  (USB depolama yok: depolama testleri atlandi)\n"); }
    else {
        UINT8 *a = kmalloc(4096), *c = kmalloc(4096);
        check(b->sectors > 0, "usb0: kapasite okundu");
        int ok = a && c && blk_read(b, 0, 1, a) == 0 && blk_read(b, 0, 1, c) == 0;
        check(ok && !memcmp(a, c, 512), "usb0: sektor 0 iki kez okununca ayni");
        check(ok && a[510] == 0x55 && a[511] == 0xAA, "usb0: gecerli boot imzasi");
        check(a && blk_read(b, 0, 8, a) == 0, "usb0: 8 sektor birden okuma");
        check(a && blk_read(b, b->sectors - 1, 1, a) == 0, "usb0: son sektor okunabildi");
        check(a && blk_read(b, b->sectors, 1, a) != 0, "usb0: disk sinirinin otesi reddedildi");
        UINT64 last = b->sectors - 1;
        UINT8 *orig = kmalloc(512), *pat = kmalloc(512), *back = kmalloc(512);
        int wr = orig && pat && back && blk_read(b, last, 1, orig) == 0;
        for (int i = 0; wr && i < 512; i++) pat[i] = (UINT8)(i * 5 + 77);
        wr = wr && blk_write(b, last, 1, pat) == 0 && blk_read(b, last, 1, back) == 0 && !memcmp(pat, back, 512);
        check(wr, "usb0: yazma: son sektor yazildi ve geri okundu");
        if (orig) blk_write(b, last, 1, orig);
        check(orig && back && blk_read(b, last, 1, back) == 0 && !memcmp(orig, back, 512), "usb0: orijinal veri geri yuklendi");
        kfree(orig); kfree(pat); kfree(back);

        fs_dirent_t e;
        check(fs_stat("/mnt/usb0", &e) == 0 && e.is_dir, "/mnt/usb0: FAT hacmi baglandi");
        if (fs_stat("/mnt/usb0/USB.TXT", &e) == 0) {
            UINT64 sz = 0;
            char *txt = fs_read_all("/mnt/usb0/USB.TXT", &sz);
            check(txt && sz > 0, "USB.TXT diskten okundu");
            kfree(txt);
        }
        // dosya sistemi yazma: USB diskte dosya olustur / oku / sil
        int w = fs_write_file("/mnt/usb0/deos_usb_test.txt", "usb yazma testi", 15);
        UINT64 sz2 = 0;
        char *back2 = w == 0 ? fs_read_all("/mnt/usb0/deos_usb_test.txt", &sz2) : 0;
        check(w == 0 && back2 && sz2 == 15 && !memcmp(back2, "usb yazma testi", 15), "FAT yazma: USB diskte dosya olusturuldu ve okundu");
        kfree(back2);
        check(fs_remove("/mnt/usb0/deos_usb_test.txt") == 0, "FAT: dosya silindi");
        kfree(a); kfree(c);
    }
    kprintf(g_ok ? "Sonuc: TUM TESTLER GECTI\n" : "Sonuc: BAZI TESTLER BASARISIZ\n");
    return g_ok;
}

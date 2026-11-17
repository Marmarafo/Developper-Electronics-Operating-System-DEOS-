// DEOS -- cekirdek: baslatma sirasi, metin konsolu kabugu, panik ekrani.
// UEFI'den tamamen bagimsiz, saf freestanding C. Giris noktasi entry.S'teki
// _start'tir (kendi stack'ini kurar), oradan kernel_main cagrilir.
#include "types.h"
#include "bootinfo.h"
#include "gdt.h"
#include "idt.h"
#include "pic.h"
#include "irq.h"
#include "timer.h"
#include "keyboard.h"
#include "console.h"
#include "serial.h"
#include "pmm.h"
#include "heap.h"
#include "selftest.h"
#include "paging.h"
#include "thread.h"
#include "process.h"
#include "syscall.h"
#include "wm.h"
#include "rtc.h"
#include "mouse.h"
#include "pci.h"
#include "ahci.h"
#include "nvme.h"
#include "net.h"
#include "iommu.h"
#include "acpi.h"
#include "blk.h"
#include "fs.h"
#include "usb.h"
#include "config.h"
#include "cpu.h"
#include "kprintf.h"
#include "string.h"
#include "io.h"
#include "rand.h"
#include "syscall_abi.h"

// Renk paleti (0x00RRGGBB)
#define C_BG     0x001A1B26u
#define C_FG     0x00C0CAF5u
#define C_GREEN  0x009ECE6Au
#define C_RED    0x00F7768Eu
#define C_YELLOW 0x00E0AF68u
#define C_CYAN   0x007DCFFFu
#define C_GRAY   0x00737AA2u
#define C_BAR_BG 0x00283457u
#define C_PANIC  0x00800000u



extern char __kernel_start[], __kernel_end[];
extern char __text_start[], __text_end[], __rodata_start[], __rodata_end[], __data_start[], __data_end[];

static BootInfo *g_bi;

static const char *g_exc_names[32] = {
    "Divide Error (#DE)", "Debug (#DB)", "NMI", "Breakpoint (#BP)",
    "Overflow (#OF)", "BOUND Range (#BR)", "Invalid Opcode (#UD)", "Device Not Available (#NM)",
    "Double Fault (#DF)", "Coprocessor Segment Overrun", "Invalid TSS (#TS)", "Segment Not Present (#NP)",
    "Stack Fault (#SS)", "General Protection (#GP)", "Page Fault (#PF)", "Reserved",
    "x87 FPU Error (#MF)", "Alignment Check (#AC)", "Machine Check (#MC)", "SIMD FP Exception (#XM)",
    "Virtualization (#VE)", "Control Protection (#CP)", "Reserved", "Reserved",
    "Reserved", "Reserved", "Reserved", "Reserved",
    "Hypervisor Injection (#HV)", "VMM Communication (#VC)", "Security (#SX)", "Reserved"
};

const char *kernel_exception_name(UINT64 vector) {
    return vector < 32 ? g_exc_names[vector] : "?";
}

// idt.c icindeki isr_handler bunu cagirir (CPU exception). Artik gercek metin
// gosteriyoruz: exception adi, hata kodu, RIP (ve page fault'ta CR2).
void kernel_panic(UINT64 vector, UINT64 error_code, UINT64 rip) {
    __asm__ __volatile__ ("cli");
    UINT64 cr2;
    __asm__ __volatile__ ("mov %%cr2, %0" : "=r"(cr2));

    console_full_clear(C_PANIC);
    console_set_color(0x00FFFFFFu, C_PANIC);
    kprintf("\n  *** KERNEL PANIC ***\n\n");
    kprintf("  Exception %llu: %s\n", vector, vector < 32 ? g_exc_names[vector] : "?");
    kprintf("  Hata kodu : 0x%llx\n", error_code);
    kprintf("  RIP       : 0x%016llx\n", rip);
    if (vector == 8) {
        UINT64 ph, fl; UINT32 sh;
        kprintf("  CR2       : 0x%016llx\n", cr2);
        if (!paging_query(cr2, &ph, &fl, &sh))
            kprintf("  Teshis    : STACK TASMASI -- CR2 eslenmemis bir sayfada (koruma sayfasi)\n");
        else
            kprintf("  Teshis    : kesme/exception islenirken ikinci bir exception olustu\n");
        kprintf("  (Bu ekran #DF icin ayrilmis IST stack'inde calisiyor)\n");
    }
    if (vector == 14) {
        kprintf("  CR2       : 0x%016llx  (erisilmek istenen adres)\n", cr2);
        kprintf("  Sebep     : %s\n", (error_code & 1) ? "koruma ihlali (sayfa var)" : "sayfa ESLENMEMIS");
        kprintf("  Erisim    : %s, %s modunda%s\n",
                (error_code & 16) ? "KOMUT GETIRME (NX)" : ((error_code & 2) ? "YAZMA" : "OKUMA"),
                (error_code & 4) ? "kullanici" : "kernel",
                (error_code & 8) ? ", ayrilmis bit" : "");
        UINT64 cr4v;
        __asm__ __volatile__ ("mov %%cr4, %0" : "=r"(cr4v));
        if (((cr4v >> 21) & 1) && (error_code & 1) && !(error_code & 4) && cr2 >= USER_BASE && cr2 < USER_TOP)
            kprintf("  Teshis    : SMAP IHLALI -- kernel, kullanici sayfasina dogrudan erismeye calisti\n"
                    "              (kullanici verisi icin copy_from_user / copy_to_user kullanilmali)\n");
    }
    kprintf("\n  Sistem durduruldu. Yeniden baslatmak icin QEMU'yu kapatip acin.\n");
    while (1) { __asm__ __volatile__ ("hlt"); }
}

// Exception disi (yazilimsal) panik: yigin canary'si vb. Ekrani ve seri portu kullanir.
__attribute__((noreturn)) void kernel_panic_text(const char *title, const char *detail, UINT64 rip) {
    __asm__ __volatile__ ("cli");
    console_full_clear(C_PANIC);
    console_set_color(0x00FFFFFFu, C_PANIC);
    kprintf("\n  *** KERNEL PANIC ***\n\n");
    kprintf("  %s\n", title);
    kprintf("  %s\n", detail);
    kprintf("  Donus adresi: 0x%016llx\n", rip);
    kprintf("\n  Sistem durduruldu. Yeniden baslatmak icin QEMU'yu kapatip acin.\n");
    while (1) { __asm__ __volatile__ ("hlt"); }
}

// ---------------------------------------------------------------- yardimcilar

static void log_ok(const char *msg) {
    console_set_color(C_GRAY, C_BG);  kprintf("[ ");
    console_set_color(C_GREEN, C_BG); kprintf("OK");
    console_set_color(C_GRAY, C_BG);  kprintf(" ] ");
    console_set_color(C_FG, C_BG);    kprintf("%s\n", msg);
}

static const char *g_mem_type_names[15] = {
    "Reserved", "LoaderCode", "LoaderData", "BootServicesCode", "BootServicesData",
    "RuntimeServicesCode", "RuntimeServicesData", "Conventional", "Unusable",
    "ACPIReclaim", "ACPINVS", "MMIO", "MMIOPortSpace", "PalCode", "Persistent"
};

// UEFI bellek haritasi tanimlayicisi: +0 Type(u32), +8 PhysicalStart, +24 NumberOfPages.
// Adim boyutu bootloader'dan gelen desc_size ile belirlenir (sabit 48 varsayilmaz).
static UINT64 mem_collect(UINT64 pages_by_type[16]) {
    for (int i = 0; i < 16; i++) pages_by_type[i] = 0;
    UINT64 count = g_bi->memory_map_size / g_bi->memory_map_desc_size;
    for (UINT64 i = 0; i < count; i++) {
        UINT8 *d = g_bi->memory_map + i * g_bi->memory_map_desc_size;
        UINT32 type = *(UINT32*)d;
        UINT64 pages = *(UINT64*)(d + 24);
        if (type < 16) pages_by_type[type] += pages;
    }
    return count;
}

static UINT64 usable_mb(UINT64 pages_by_type[16]) {
    // Bootloader/UEFI sonrasi geri kazanilabilir turler: LoaderCode/Data,
    // BootServicesCode/Data ve Conventional.
    UINT64 pages = pages_by_type[1] + pages_by_type[2] + pages_by_type[3] +
                   pages_by_type[4] + pages_by_type[7];
    return pages * 4096 / (1024 * 1024);
}

static void status_bar_update(UINT64 ticks) {
    UINT64 s = ticks / TIMER_HZ;
    char left[64], right[64], line[256];
    ksnprintf(left, sizeof left, " DEOS " DEOS_VERSION "  |  x86_64 UEFI  |  %d thread", thread_count());
    ksnprintf(right, sizeof right, "uptime %02llu:%02llu:%02llu ",
              s / 3600, (s / 60) % 60, s % 60);
    UINT32 cols = console_cols();
    if (cols > sizeof(line) - 1) cols = sizeof(line) - 1;
    UINT32 ll = (UINT32)strlen(left), rl = (UINT32)strlen(right);
    for (UINT32 i = 0; i < cols; i++) line[i] = ' ';
    line[cols] = 0;
    for (UINT32 i = 0; i < ll && i < cols; i++) line[i] = left[i];
    if (rl < cols) for (UINT32 i = 0; i < rl; i++) line[cols - rl + i] = right[i];
    console_put_at(0, 0, line, 0x00FFFFFFu, C_BAR_BG);
}

// ---------------------------------------------------------------------- shell

static UINT64 g_last_sec = ~0ULL;
static int    g_cursor_state = -1;

// Durum cubugunu (saniyede bir) ve imlec yanip sonmesini gunceller.
static void shell_tick(void) {
    UINT64 t = timer_get_ticks();
    if (t / TIMER_HZ != g_last_sec) { g_last_sec = t / TIMER_HZ; status_bar_update(t); }
    int blink = (int)((t / 50) % 2);
    if (blink != g_cursor_state) { g_cursor_state = blink; console_cursor(blink); }
}


#define LINE_MAX 120
static char g_cwd[128] = "/";
static char g_line[LINE_MAX];
static UINT32 g_len;

static void prompt(void) {
    console_set_color(C_GREEN, C_BG); kprintf("deos");
    console_set_color(C_GRAY, C_BG);  kprintf(":");
    console_set_color(C_CYAN, C_BG);  kprintf("%s", g_cwd);
    console_set_color(C_GRAY, C_BG);  kprintf("> ");
    console_set_color(C_FG, C_BG);
}

static void cmd_help(void) {
    kprintf("Komutlar:\n");
    kprintf("  help          bu listeyi gosterir\n");
    kprintf("  clear         ekrani temizler\n");
    kprintf("  info          sistem/ekran bilgisi\n");
    kprintf("  mem           bellek haritasi ozeti\n");
    kprintf("  uptime        calisma suresi\n");
    kprintf("  date          tarih ve saat (RTC)\n");
    kprintf("  mouse         fare surucusu tanilama bilgisi\n");
    kprintf("  pmm           fiziksel bellek yoneticisi durumu\n");
    kprintf("  heap          kernel heap durumu\n");
    kprintf("  memtest       PMM + heap otomatik testi\n");
    kprintf("  hwtest [write]  ACPI/CPU/klavye/ELF/ag/NVMe testleri   selftest [write]  TUM testler\n");
    kprintf("  ping A.B.C.D  netinfo   netcfg A.B.C.D [maske] [gecit]   ag: ICMP echo / NIC durumu / elle yapilandirma\n");
    kprintf("  dhcp          DHCP ile adres al      nslookup <ad>   DNS sorgusu      netstat   TCP baglantilari\n");
    kprintf("  iommu         IOMMU (DMAR/IVRS) bilgisi; yalnizca raporlama\n");
    kprintf("  acpi          ACPI/MADT: CPU, APIC, IRQ yonlendirme bilgisi\n");
    kprintf("  echo <metin>  metni yazdirir\n");
    kprintf("  vm            sayfalama (sanal bellek) durumu\n");
    kprintf("  vmtest        sayfalama otomatik testi\n");
    kprintf("  crash [tur]   kasitli hata: div|null|write|rodata|nx|stack|tstack|smap|canary\n");
    kprintf("  ps            thread listesi\n");
    kprintf("  spawn         arka planda CPU yiyen bir thread baslatir\n");
    kprintf("  kill <id>     thread sonlandirir\n");
    kprintf("  threadtest    zamanlayici otomatik testi\n");
    kprintf("  progs         gomulu kullanici programlari  (diger programlar: ls /bin)\n");
    kprintf("  systest       ring 3 dosya/surec syscall testi    run sh   ring 3 kabugu\n");
    kprintf("  run <prog>    programi ring 3'te calistirir, bitmesini bekler\n");
    kprintf("  bg <prog>     programi arka planda baslatir\n");
    kprintf("  usertest      kullanici modu / surec izolasyonu testi\n");
    kprintf("  ls [yol]      dizin listesi        cd <yol> / pwd   dizin degistir / goster\n");
    kprintf("  cat <dosya>   dosya icerigi        disks / mounts / lspci   donanim ve diskler\n");
    kprintf("  disktest      depolama + dosya sistemi testi (okuma)\n");
    kprintf("  lsusb         USB cihazlari     usbtest   USB depolama testi\n");
    kprintf("  touch/mkdir/rm/rmdir <yol>   dosya-dizin olustur / sil\n");
    kprintf("  write|append <dosya> <metin>  dosyaya yazar    cp|mv <kaynak> <hedef>   kopyala / tasi\n");
    kprintf("  df            disk kullanimi       fstest   dosya sistemi yazma testi\n");
    kprintf("  reboot        yeniden baslatir\n");
}

static void cmd_info(void) {
    kprintf("Ekran        : %ux%u, framebuffer 0x%llx\n", g_bi->width, g_bi->height, g_bi->framebuffer_base);
    kprintf("Konsol       : %u x %u karakter\n", console_cols(), console_rows());
    kprintf("Kernel       : 0x%llx - 0x%llx (%llu KB)\n", (UINT64)(UINTN)__kernel_start,
            (UINT64)(UINTN)__kernel_end, ((UINT64)(UINTN)__kernel_end - (UINT64)(UINTN)__kernel_start) / 1024);
    kprintf("Timer        : PIT, %u Hz, tick=%llu\n", TIMER_HZ, timer_get_ticks());
}

static void cmd_mem(void) {
    UINT64 pages[16];
    UINT64 regions = mem_collect(pages);
    kprintf("UEFI bellek haritasi: %llu bolge\n", regions);
    for (int t = 0; t < 15; t++) {
        if (!pages[t]) continue;
        UINT64 kb = pages[t] * 4;
        if (kb >= 1024) kprintf("  %-20s %8llu MB\n", g_mem_type_names[t], kb / 1024);
        else            kprintf("  %-20s %8llu KB\n", g_mem_type_names[t], kb);
    }
    kprintf("Kullanilabilir RAM (geri kazanilabilir): %llu MB\n", usable_mb(pages));
}

static void cmd_pmm(void) {
    pmm_stats_t s;
    pmm_get_stats(&s);
    kprintf("Fiziksel bellek yoneticisi (sayfa = 4 KB):\n");
    kprintf("  Adres araligi    : %llu MB\n", s.range_pages * PAGE_SIZE / (1024 * 1024));
    kprintf("  Kullanilabilir   : %llu MB (%llu sayfa)\n", s.usable_pages * PAGE_SIZE / (1024 * 1024), s.usable_pages);
    kprintf("  Bos              : %llu MB (%llu sayfa)\n", s.free_pages * PAGE_SIZE / (1024 * 1024), s.free_pages);
    kprintf("  Kullanimda       : %llu KB\n", (s.usable_pages - s.free_pages) * 4);
    kprintf("  Bitmap           : %llu KB @ 0x%llx\n", s.bitmap_bytes / 1024, s.bitmap_addr);
}

static void cmd_heap(void) {
    heap_stats_t h;
    heap_get_stats(&h);
    kprintf("Kernel heap:\n");
    kprintf("  Toplam      : %llu KB\n", h.total_bytes / 1024);
    kprintf("  Kullanilan  : %llu bayt\n", h.used_bytes);
    kprintf("  Bos         : %llu KB\n", h.free_bytes / 1024);
    kprintf("  Bloklar     : %llu (%llu bos)\n", h.blocks, h.free_blocks);
}

static void cmd_vm(void) {
    paging_info_t pi;
    paging_get_info(&pi);
    kprintf("Sayfalama (4 seviyeli, PML4):\n");
    kprintf("  CR3             : 0x%llx\n", pi.cr3);
    kprintf("  Birebir esleme  : 0 - %llu GB (2 MB sayfalar)\n", pi.mapped_top / (1024ULL * 1024 * 1024));
    kprintf("  Tablo sayfalari : %llu (%llu KB)\n", pi.table_pages, pi.table_pages * 4);
    kprintf("  Ozellikler      : NX %s, CR0.WP %s, PAT write-combining %s\n",
            pi.nx ? "acik" : "yok", pi.wp ? "acik" : "kapali", pi.wc ? "(framebuffer)" : "kapali");
    kprintf("  Kernel bolumleri:\n");
    kprintf("    .text   0x%llx - 0x%llx  R-X\n", (UINT64)(UINTN)__text_start, (UINT64)(UINTN)__text_end);
    kprintf("    .rodata 0x%llx - 0x%llx  R--\n", (UINT64)(UINTN)__rodata_start, (UINT64)(UINTN)__rodata_end);
    kprintf("    .data   0x%llx - 0x%llx  RW-\n", (UINT64)(UINTN)__data_start, (UINT64)(UINTN)__data_end);
    kprintf("  Sayfa 0         : eslenmemis (NULL korumasi)\n");
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winfinite-recursion"
static void recurse(volatile int n) {
    volatile char buf[512];
    buf[0] = (char)n;
    recurse(n + 1);                 // sonsuz ozyineleme -> stack tasar
    buf[1] = buf[0];
}
#pragma GCC diagnostic pop

static void overflow_thread(void *arg) { (void)arg; recurse(0); }

// SMAP denemesi: kullanici sayfasi (U=1) eslenmis bir adres uzayina gecip, stac OLMADAN dogrudan okur.
// SMAP aciksa #PF (SMAP ihlali) ile panik beklenir.
static void crash_smap(void) {
    if (!syscall_smap_enabled()) { kprintf("SMAP etkin degil (islemci CPUID.7.0:EBX[20] bildirmiyor); deneme yapilmadi.\n"); return; }
    UINT64 cr3u = paging_new_user_space();
    UINT64 pg = pmm_alloc_page();
    if (!cr3u || !pg) { kprintf("bellek yok\n"); return; }
    memset((void*)(UINTN)pg, 0, PAGE_SIZE);
    *(volatile UINT64*)(UINTN)pg = 0x1122334455667788ULL;
    if (paging_map_in(cr3u, USER_BASE + 0x1000, pg, PTE_U | PTE_W | PTE_NX) != 0) { kprintf("esleme basarisiz\n"); return; }
    UINT64 old; __asm__ __volatile__ ("mov %%cr3, %0" : "=r"(old));
    kprintf("kernel, kullanici sayfasini stac olmadan okuyor... (panik beklenir)\n");
    __asm__ __volatile__ ("cli");
    __asm__ __volatile__ ("mov %0, %%cr3" : : "r"(cr3u) : "memory");
    volatile UINT64 x = *(volatile UINT64*)(USER_BASE + 0x1000);      // SMAP -> #PF
    __asm__ __volatile__ ("mov %0, %%cr3" : : "r"(old) : "memory");
    __asm__ __volatile__ ("sti");
    kprintf("HATA: SMAP engellemedi (okunan 0x%llx)\n", (UINT64)x);
}

// Yigin canary denemesi: yerel tamponu tasirir; islev donerken canary kontrolu panik vermelidir.
__attribute__((noinline)) static void canary_victim(volatile int n) {
    volatile char buf[16];
    volatile char *p = buf;
    for (int i = 0; i < n; i++) p[i] = 0x41;
}

static void netstat_cmd(void) {
    net_stats_t s; net_get_stats(&s);
    kprintf("TCP baglanti sayisi: %d\n", tcp_conn_count());
    kprintf("cerceve: alinan %llu (atilan %llu), gonderilen %llu (atilan %llu)\n", s.rx_frames, s.rx_dropped, s.tx_frames, s.tx_dropped);
}

static void cmd_crash(const char *what) {
    if (!*what || !strcmp(what, "div")) {
        volatile int a = 1, b = 0; volatile int c = a / b; (void)c;       // #DE
    } else if (!strcmp(what, "null")) {
        volatile UINT64 *p = (volatile UINT64*)0; volatile UINT64 x = *p; (void)x;   // NULL okuma -> #PF
    } else if (!strcmp(what, "write")) {
        *(volatile UINT8*)(UINTN)__text_start = 0x90;                    // .text'e yazma -> #PF (WP)
    } else if (!strcmp(what, "rodata")) {
        *(volatile char*)g_exc_names[0] = 'X';                            // .rodata'ya yazma -> #PF
    } else if (!strcmp(what, "nx")) {
        UINT8 *code = kmalloc(16);                                        // heap'te kod calistirma -> #PF (NX)
        if (code) { code[0] = 0xC3; ((void (*)(void))code)(); }
    } else if (!strcmp(what, "stack")) {
        recurse(0);                                                       // shell stack'i tasar -> #DF (IST)
    } else if (!strcmp(what, "smap")) {
        crash_smap();
    } else if (!strcmp(what, "canary")) {
        kprintf("yerel tampon tasiriliyor... (panik beklenir)\n");
        canary_victim(64);
        kprintf("HATA: canary tasmayi yakalamadi\n");
    } else if (!strcmp(what, "tstack")) {
        thread_create("overflow", overflow_thread, 0);                    // thread stack'i tasar -> #DF (IST)
        thread_sleep_ms(1000);
    } else {
        kprintf("Kullanim: crash [div|null|write|rodata|nx|stack|tstack|smap|canary]\n");
    }
}

static void spinner(void *arg) { (void)arg; volatile UINT64 n = 0; for (;;) n++; }

static int parse_uint(const char *s) {
    int v = 0, any = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; any = 1; }
    return any ? v : -1;
}

static void cmd_spawn(void) {
    static int count = 0;
    char name[16];
    ksnprintf(name, sizeof name, "spin%d", ++count);
    int id = thread_create(name, spinner, 0);
    if (id < 0) kprintf("thread olusturulamadi\n");
    else kprintf("thread %d (%s) baslatildi; 'ps' ile bakin, 'kill %d' ile durdurun\n", id, name, id);
}

static void cmd_kill(const char *arg) {
    int id = parse_uint(arg);
    if (id < 0) { kprintf("Kullanim: kill <id>\n"); return; }
    if (thread_kill(id) == 0) kprintf("thread %d sonlandirildi\n", id);
    else kprintf("thread %d sonlandirilamadi (yok, shell veya idle)\n", id);
}

static void cmd_progs(void) {
    kprintf("Gomulu kullanici programlari (ELF, ring 3):\n");
    for (const embedded_prog_t *p = g_progs; p->name; p++)
        kprintf("  %-10s %llu bayt\n", p->name, (UINT64)(p->end - p->start));
}

static void resolve(const char *arg, char *out, UINT32 cap) {
    char tmp[160];
    size_t n = strlen(arg);
    if (n >= 2 && arg[0] == '"' && arg[n - 1] == '"' && n < sizeof tmp) { memcpy(tmp, arg + 1, n - 2); tmp[n - 2] = 0; arg = tmp; }
    fs_normalize(g_cwd, arg, out, cap);
}

static void cmd_ls(const char *arg) {
    char path[160];
    resolve(*arg ? arg : ".", path, sizeof path);
    fs_dirent_t e;
    if (fs_stat(path, &e) != 0) { kprintf("ls: '%s' bulunamadi\n", path); return; }
    if (!e.is_dir) { kprintf("  %-32s %10u bayt\n", e.name, e.size); return; }
    int n = 0;
    for (int i = 0; fs_readdir(path, i, &e) == 0; i++, n++) {
        if (e.is_dir) { console_set_color(C_CYAN, C_BG); kprintf("  %-32s <DIZIN>\n", e.name); console_set_color(C_FG, C_BG); }
        else kprintf("  %-32s %10u bayt\n", e.name, e.size);
    }
    kprintf("  %d girdi\n", n);
}

static void cmd_cd(const char *arg) {
    char path[160];
    resolve(*arg ? arg : "/", path, sizeof path);
    fs_dirent_t e;
    if (fs_stat(path, &e) != 0 || !e.is_dir) { kprintf("cd: '%s' dizin degil veya yok\n", path); return; }
    size_t n = strlen(path); if (n >= sizeof g_cwd) n = sizeof g_cwd - 1;
    memcpy(g_cwd, path, n); g_cwd[n] = 0;
}

static void cmd_cat(const char *arg) {
    if (!*arg) { kprintf("Kullanim: cat <dosya>\n"); return; }
    char path[160];
    resolve(arg, path, sizeof path);
    fs_dirent_t e;
    if (fs_stat(path, &e) != 0) { kprintf("cat: '%s' bulunamadi\n", path); return; }
    if (e.is_dir) { kprintf("cat: '%s' bir dizin\n", path); return; }
    UINT64 want = e.size > 8192 ? 8192 : e.size;
    char *buf = kmalloc(want + 1);
    if (!buf) { kprintf("cat: bellek yok\n"); return; }
    long long got = fs_read(path, 0, buf, want);
    if (got < 0) { kprintf("cat: okuma hatasi\n"); kfree(buf); return; }
    for (long long i = 0; i < got; i++) {
        char c = buf[i];
        if (c == '\n' || c == '\t' || (c >= 32 && c < 127)) console_putc(c);
        else if (c != '\r') console_putc('.');
    }
    if (got && buf[got - 1] != '\n') console_putc('\n');
    if (e.size > want) kprintf("... (%u bayttan ilk %llu bayt gosterildi)\n", e.size, want);
    kfree(buf);
}

static void cmd_disks(void) {
    if (!blk_count()) { kprintf("Disk bulunamadi.\n"); return; }
    kprintf("  AYGIT   BOYUT       MODEL\n");
    for (int i = 0; i < blk_count(); i++) {
        blockdev_t *d = blk_get(i);
        UINT64 mb = d->sectors / 2048;
        kprintf("  %-7s %6llu MB   %s%s%s\n", d->name, mb, d->model, d->parent ? "  (bolum)" : "", d->read ? "" : "  (cikarildi)");
    }
}

static void cmd_mounts(void) {
    if (!fs_mount_count()) { kprintf("Bagli dosya sistemi yok.\n"); return; }
    for (int i = 0; i < fs_mount_count(); i++) {
        const char *mp, *dev; int t;
        const char *label = fs_mount_info(i, &mp, &dev, &t);
        kprintf("  %-14s %-7s FAT%d  etiket: %s\n", mp, dev, t, label[0] ? label : "(yok)");
    }
}

static void cmd_lspci(void) {
    kprintf("  YOL     SATICI:AYGIT  SINIF\n");
    for (int i = 0; i < pci_count(); i++) {
        const pci_dev_t *d = pci_get(i);
        kprintf("  %02x:%02x.%x %04x:%04x     %s\n", d->bus, d->dev, d->fn, d->vendor, d->device, pci_class_name(d->class_code, d->subclass));
    }
}

// "ilk_kelime geri_kalan" -> iki parca (yerinde, NUL ile bolunur). Ilk kelime "tirnak icinde" olabilir (bosluklu adlar icin).
static void split2(char *args, char **a, char **b) {
    char *p = args;
    if (*p == '"') {
        *a = ++p;
        while (*p && *p != '"') p++;
        if (*p) *p++ = 0;
    } else {
        *a = p;
        while (*p && *p != ' ') p++;
        if (*p) *p++ = 0;
    }
    while (*p == ' ') p++;
    *b = p;
}

static void fs_err(const char *what, const char *path, int r) { kprintf("%s: '%s': %s\n", what, path, fs_strerror(r)); }

static void cmd_touch(const char *arg) {
    if (!*arg) { kprintf("Kullanim: touch <dosya>\n"); return; }
    char p[160]; resolve(arg, p, sizeof p);
    fs_dirent_t e;
    if (fs_stat(p, &e) == 0) return;                                  // zaten var
    int r = fs_create(p, 0);
    if (r) fs_err("touch", p, r);
}

static void cmd_mkdir(const char *arg) {
    if (!*arg) { kprintf("Kullanim: mkdir <dizin>\n"); return; }
    char p[160]; resolve(arg, p, sizeof p);
    int r = fs_create(p, 1);
    if (r) fs_err("mkdir", p, r);
}

static void cmd_rm(const char *arg, int want_dir) {
    if (!*arg) { kprintf("Kullanim: %s <yol>\n", want_dir ? "rmdir" : "rm"); return; }
    char p[160]; resolve(arg, p, sizeof p);
    fs_dirent_t e;
    if (fs_stat(p, &e) != 0) { kprintf("%s: '%s' bulunamadi\n", want_dir ? "rmdir" : "rm", p); return; }
    if (e.is_dir != want_dir) { kprintf("%s: '%s' %s\n", want_dir ? "rmdir" : "rm", p, want_dir ? "bir dizin degil" : "bir dizin (rmdir kullanin)"); return; }
    int r = fs_remove(p);
    if (r) fs_err(want_dir ? "rmdir" : "rm", p, r);
}

static void cmd_write(char *args, int append) {
    char *f, *text;
    split2(args, &f, &text);
    if (!*f) { kprintf("Kullanim: %s <dosya> <metin>\n", append ? "append" : "write"); return; }
    char p[160]; resolve(f, p, sizeof p);
    UINT32 n = (UINT32)strlen(text);
    char *buf = kmalloc(n + 2);
    if (!buf) { kprintf("bellek yok\n"); return; }
    memcpy(buf, text, n); buf[n++] = '\n';
    int r;
    fs_dirent_t e;
    if (append) {
        r = 0;
        if (fs_stat(p, &e) != 0) r = fs_create(p, 0), e.size = 0;
        if (r == 0) { long long w = fs_write(p, e.size, buf, n); r = w == (long long)n ? 0 : (w < 0 ? (int)w : -4); }
    } else r = fs_write_file(p, buf, n);
    kfree(buf);
    if (r) fs_err(append ? "append" : "write", p, r);
}

// hedef bir dizinse kaynagin adini ekler
static void dest_path(const char *src, const char *arg, char *out, UINT32 cap) {
    resolve(arg, out, cap);
    fs_dirent_t e;
    if (fs_stat(out, &e) == 0 && e.is_dir) {
        const char *base = strrchr(src, '/');
        base = base ? base + 1 : src;
        UINT32 l = (UINT32)strlen(out);
        if (l + 2 + strlen(base) < cap) { if (l > 1) out[l++] = '/'; memcpy(out + l, base, strlen(base) + 1); }
    }
}

static void cmd_cp_mv(char *args, int move) {
    char *a, *b;
    split2(args, &a, &b);
    if (!*a || !*b) { kprintf("Kullanim: %s <kaynak> <hedef>\n", move ? "mv" : "cp"); return; }
    char src[160], dst[160];
    resolve(a, src, sizeof src);
    dest_path(src, b, dst, sizeof dst);
    fs_dirent_t e;
    if (fs_stat(src, &e) != 0) { kprintf("%s: '%s' bulunamadi\n", move ? "mv" : "cp", src); return; }
    if (move) {
        int r = fs_rename(src, dst);
        if (r) fs_err("mv", dst, r);
        return;
    }
    if (e.is_dir) { kprintf("cp: dizin kopyalama desteklenmiyor\n"); return; }
    UINT64 size;
    void *data = fs_read_all(src, &size);
    if (!data) { kprintf("cp: '%s' okunamadi\n", src); return; }
    int r = fs_write_file(dst, data, size);
    kfree(data);
    if (r) fs_err("cp", dst, r); else kprintf("%llu bayt kopyalandi\n", size);
}

static void cmd_df(void) {
    kprintf("  %-14s %-7s %-6s %9s %9s %9s\n", "BAGLAMA", "AYGIT", "TUR", "TOPLAM", "KULLANILAN", "BOS");
    for (int i = 0; i < fs_mount_count(); i++) {
        const char *mp, *dev; int t;
        fs_mount_info(i, &mp, &dev, &t);
        UINT64 total, freeb;
        if (fs_space(mp, &total, &freeb) != 0) continue;
        kprintf("  %-14s %-7s FAT%-3d %6llu MB %6llu MB %6llu MB\n", mp, dev, t, total >> 20, (total - freeb) >> 20, freeb >> 20);
    }
}

static void cmd_run(char *args, int wait) {
    if (!*args) { kprintf("Kullanim: %s <program> [argumanlar]   (liste: progs, ls /bin)\n", wait ? "run" : "bg"); return; }
    char *argv[MAX_ARGS + 1];
    int argc = 0;
    for (char *p = args; *p && argc < MAX_ARGS; ) {
        while (*p == ' ') p++;
        if (!*p) break;
        if (*p == '"') { argv[argc++] = ++p; while (*p && *p != '"') p++; }
        else           { argv[argc++] = p;   while (*p && *p != ' ') p++; }
        if (*p) *p++ = 0;
    }
    argv[argc] = 0;
    const char *name = argv[0];
    int pid = -1;
    if (!strchr(name, '/')) pid = process_spawn_embedded(name, argc, argv, g_cwd, 0, 0);      // once gomulu programlar
    if (pid == -1) {                                                                          // sonra dosya sistemi
        char path[160];
        resolve(name, path, sizeof path);
        pid = process_spawn_file(path, argc, argv, g_cwd, 0, 0);
        if (pid == -1 && !strchr(name, '/')) {
            ksnprintf(path, sizeof path, "/bin/%s.elf", name);
            pid = process_spawn_file(path, argc, argv, g_cwd, 0, 0);
        }
    }
    if (pid == -1) { kprintf("Program bulunamadi: %s  ('progs' veya 'ls /bin')\n", name); return; }
    if (pid < 0)   { kprintf("Program yuklenemedi: %s\n", name); return; }
    if (!wait) { kprintf("[pid %d] %s arka planda baslatildi\n", pid, name); return; }
    while (thread_alive(pid)) { if (!wm_active()) shell_tick(); thread_sleep_ms(10); }
    int code = 0;
    thread_get_exit_code(pid, &code);
    console_set_color(code == 0 ? C_GREEN : C_RED, C_BG);
    kprintf("[pid %d] %s bitti, cikis kodu: %d%s\n", pid, name, code, code == -1 ? "  (hata nedeniyle sonlandirildi)" : "");
    console_set_color(C_FG, C_BG);
    g_cursor_state = -1;
}

static void cmd_date(void) {
    rtc_time_t t;
    rtc_get(&t);
    kprintf("%02d.%02d.%04d %02d:%02d:%02d\n", t.day, t.month, t.year, t.hour, t.min, t.sec);
}

static void cmd_mouse(void) {
    mouse_stats_t m;
    mouse_get_stats(&m);
    kprintf("PS/2 fare: init %s, i8042 cfg=0x%x\n", m.init_ok ? "BASARILI" : "BASARISIZ", m.cfg);
    kprintf("  IRQ12: %u, bayt: %u, paket: %u, atilan: %u\n", m.irqs, m.bytes, m.packets, m.dropped);
    kprintf("  son paket: dx=%d dy=%d dugmeler=%d\n", m.last_dx, m.last_dy, m.last_buttons);
}

static void cmd_lsusb(void) {
    kprintf("  PORT  HIZ           VID:PID    TUR                    RAPOR (hata)\n");
    usbdev_t *d;
    int n = 0;
    for (int i = 0; (d = usb_device(i)); i++, n++) {
        char path[24]; int ports[8], np = 0;                        // kok port, sonra hub portlari: "4.1", "4.2.3"
        for (const usbdev_t *x = d; x && np < 8; x = x->parent) ports[np++] = x->parent ? x->parent_port : x->port;
        int o = 0; for (int k = np - 1; k >= 0; k--) o += ksnprintf(path + o, sizeof path - (size_t)o, k == np - 1 ? "%d" : ".%d", ports[k]);
        kprintf("  %-5s %-13s %04x:%04x  %-22s %u (%u)\n", path, usb_speed_name(d->speed), d->vid, d->pid,
                d->removed ? "(cikarildi)" : usb_kind_name(d->kind), d->reports, d->errors);
    }
    if (!n) kprintf("  (USB cihazi yok)\n");
    kprintf("  xHCI olay sayisi: %u\n", usb_event_count());
}

static void cmd_uptime(void) {
    UINT64 t = timer_get_ticks();
    kprintf("%llu saniye (%llu tick)\n", t / TIMER_HZ, t);
}

static void run_command(char *line) {
    while (*line == ' ') line++;
    if (!*line) return;
    char *args = line;
    while (*args && *args != ' ') args++;
    if (*args) { *args++ = 0; while (*args == ' ') args++; }

    if      (!strcmp(line, "help"))   cmd_help();
    else if (!strcmp(line, "clear"))  console_clear();
    else if (!strcmp(line, "info"))   cmd_info();
    else if (!strcmp(line, "mem"))    cmd_mem();
    else if (!strcmp(line, "uptime")) cmd_uptime();
    else if (!strcmp(line, "date"))   cmd_date();
    else if (!strcmp(line, "mouse"))  cmd_mouse();
    else if (!strcmp(line, "lsusb"))  cmd_lsusb();
    else if (!strcmp(line, "usbtest")) selftest_usb();
    else if (!strcmp(line, "pmm"))    cmd_pmm();
    else if (!strcmp(line, "heap"))   cmd_heap();
    else if (!strcmp(line, "memtest")) selftest_memory();
    else if (!strcmp(line, "echo"))   kprintf("%s\n", args);
    else if (!strcmp(line, "crash"))  cmd_crash(args);
    else if (!strcmp(line, "vm"))     cmd_vm();
    else if (!strcmp(line, "ps"))     thread_dump();
    else if (!strcmp(line, "spawn"))  cmd_spawn();
    else if (!strcmp(line, "kill"))   cmd_kill(args);
    else if (!strcmp(line, "threadtest")) selftest_threads();
    else if (!strcmp(line, "progs"))  cmd_progs();
    else if (!strcmp(line, "run"))    cmd_run(args, 1);
    else if (!strcmp(line, "bg"))     cmd_run(args, 0);
    else if (!strcmp(line, "systest")) { char a[] = "systest"; cmd_run(a, 1); }
    else if (!strcmp(line, "usertest")) selftest_user();
    else if (!strcmp(line, "ls"))     cmd_ls(args);
    else if (!strcmp(line, "cd"))     cmd_cd(args);
    else if (!strcmp(line, "pwd"))    kprintf("%s\n", g_cwd);
    else if (!strcmp(line, "cat"))    cmd_cat(args);
    else if (!strcmp(line, "disks"))  cmd_disks();
    else if (!strcmp(line, "mounts")) cmd_mounts();
    else if (!strcmp(line, "lspci"))  cmd_lspci();
    else if (!strcmp(line, "disktest")) selftest_storage(args);
    else if (!strcmp(line, "fstest")) selftest_fs();
    else if (!strcmp(line, "touch"))  cmd_touch(args);
    else if (!strcmp(line, "mkdir"))  cmd_mkdir(args);
    else if (!strcmp(line, "rm"))     cmd_rm(args, 0);
    else if (!strcmp(line, "rmdir"))  cmd_rm(args, 1);
    else if (!strcmp(line, "write"))  cmd_write(args, 0);
    else if (!strcmp(line, "append")) cmd_write(args, 1);
    else if (!strcmp(line, "cp"))     cmd_cp_mv(args, 0);
    else if (!strcmp(line, "mv"))     cmd_cp_mv(args, 1);
    else if (!strcmp(line, "df"))     cmd_df();
    else if (!strcmp(line, "vmtest")) selftest_paging(g_bi);
    else if (!strcmp(line, "hwtest"))  selftest_hw("");
    else if (!strcmp(line, "hwtest write")) selftest_hw("write");
    else if (!strcmp(line, "selftest")) selftest_all(g_bi, 0);
    else if (!strcmp(line, "selftest write")) selftest_all(g_bi, 1);
    else if (!strcmp(line, "acpi"))   acpi_dump();
    else if (!strcmp(line, "iommu"))  iommu_dump();
    else if (!strncmp(line, "ping ", 5)) net_ping(line + 5);
    else if (!strcmp(line, "netinfo")) net_info();
    else if (!strncmp(line, "netcfg ", 7)) net_set_ip(line + 7);
    else if (!strcmp(line, "dhcp")) { kprintf("DHCP: adres isteniyor...\n"); if (dhcp_acquire(12000) == 0) net_info(); else kprintf("DHCP basarisiz\n"); }
    else if (!strncmp(line, "nslookup ", 9)) {
        UINT32 ip;
        if (dns_resolve(line + 9, &ip, 6000) == 0) { char a[16]; net_format_ip(ip, a); kprintf("%s -> %s\n", line + 9, a); }
        else kprintf("nslookup: %s cozulemedi\n", line + 9);
    }
    else if (!strcmp(line, "netstat")) netstat_cmd();
    else if (!strcmp(line, "reboot")) {
        kprintf("Yeniden baslatiliyor...\n");
        acpi_reboot();   // FADT RESET_REG (ACPI 2.0+), yoksa 8042 reset
    }
    else {
        console_set_color(C_RED, C_BG);
        kprintf("Bilinmeyen komut: %s", line);
        console_set_color(C_FG, C_BG);
        kprintf("  ('help' yazin)\n");
    }
}

static void shell_key(char c) {
    if (c == '\n') {
        kprintf("\n");
        g_line[g_len] = 0;
        run_command(g_line);
        g_len = 0;
        prompt();
    } else if (c == '\b') {
        if (g_len > 0) { g_len--; console_putc('\b'); }
    } else if (c >= 32 && c < 127) {
        if (g_len < LINE_MAX - 1) { g_line[g_len++] = c; console_putc(c); }
    }
}

// ----------------------------------------------------------------------- main

void kernel_main(BootInfo *bi) {
    g_bi = bi;
    serial_init();
    rand_init();
    serial_puts("\n[kernel_main] basladi\n");
    console_init(bi);
    serial_puts("[kernel_main] console_init tamam\n");

    console_set_color(C_CYAN, C_BG);
    kprintf("DEOS - kendi isletim sistemimiz\n");
    console_set_color(C_GRAY, C_BG);
    kprintf("--------------------------------\n");
    console_set_color(C_FG, C_BG);

    log_ok("Konsol / font renderer");
    gdt_init();      log_ok("GDT yuklendi");
    idt_init();      log_ok("IDT yuklendi (32 exception + 16 IRQ)");
    pic_init();      log_ok("PIC yeniden eslendi");
    irq_init();
    timer_init(TIMER_HZ);  log_ok("PIT timer (IRQ0) 100 Hz");
    keyboard_init(); log_ok("PS/2 klavye (IRQ1)");

    pmm_init(bi);
    {
        pmm_stats_t ps; pmm_get_stats(&ps);
        kprintf("[ ");
        console_set_color(C_GREEN, C_BG); kprintf("OK");
        console_set_color(C_GRAY, C_BG);  kprintf(" ] ");
        console_set_color(C_FG, C_BG);
        kprintf("PMM: %llu MB bos bellek yonetiliyor\n", ps.free_pages * PAGE_SIZE / (1024 * 1024));
    }
    if (heap_init(8 * 1024 * 1024)) {
        heap_stats_t hs; heap_get_stats(&hs);
        char m[64]; ksnprintf(m, sizeof m, "Heap: %llu MB (kmalloc/kfree)", hs.total_bytes / (1024 * 1024));
        log_ok(m);
    } else {
        console_set_color(C_RED, C_BG); kprintf("HATA: heap kurulamadi\n"); console_set_color(C_FG, C_BG);
    }

    paging_init(bi);
    {
        paging_info_t pi; paging_get_info(&pi);
        char m[96];
        ksnprintf(m, sizeof m, "Sayfalama: kendi PML4 aktif (NX%s, WP, WC), %llu tablo sayfasi",
                  pi.nx ? "" : " yok", pi.table_pages);
        log_ok(m);
        UINT64 rec = pmm_reclaim_boot_services(bi);
        ksnprintf(m, sizeof m, "UEFI BootServices bellegi geri kazanildi: %llu MB", rec * PAGE_SIZE / (1024 * 1024));
        log_ok(m);
    }

    if (acpi_init(bi->rsdp)) {
        char m[96]; ksnprintf(m, sizeof m, "ACPI: FADT hazir (OEM %s), MADT %s", acpi_oem(), acpi_madt() ? "var" : "yok");
        log_ok(m);
    } else {
        log_ok("ACPI tablosu yok (legacy guc portlarina dusulecek)");
    }

    thread_init();
    log_ok("Zamanlayici: preemptive thread'ler (10 ms dilim) + idle thread");
    syscall_init();
    log_ok(syscall_smep_enabled() ? "Kullanici modu: syscall/sysret, ring 3, SMEP acik"
                                  : "Kullanici modu: syscall/sysret, ring 3 (SMEP yok)");
    __asm__ __volatile__ ("sti");

    pci_scan();
    {
        char m[96];
        ksnprintf(m, sizeof m, "PCI: %d aygit bulundu", pci_count());
        log_ok(m);
        int disks = ahci_init();
        int nvmes = nvme_init();
        ksnprintf(m, sizeof m, "AHCI (SATA): %d disk, NVMe: %d, toplam %d blok aygiti", disks, nvmes, blk_count());
        log_ok(m);
        int mounts = fs_init();
        if (mounts) { ksnprintf(m, sizeof m, "Dosya sistemi: %d FAT hacmi baglandi (/ = %s)", mounts, blk_count() ? "ilk hacim" : "-"); log_ok(m); }
        else        { console_set_color(C_YELLOW, C_BG); kprintf("[ -- ] Bagli dosya sistemi yok\n"); console_set_color(C_FG, C_BG); }

        iommu_detect();
        { char m[112]; ksnprintf(m, sizeof m, "IOMMU: %s", iommu_name()); log_ok(m); }

        int nnet = net_init();
        if (nnet) { ksnprintf(m, sizeof m, "Ethernet: Intel e1000 uyumlu NIC hazir (MAC %s)", net_mac_str()); log_ok(m); dhcp_boot_start(); }
        else        { log_ok("Ethernet: e1000 uyumlu NIC bulunamadi"); }

        int nusb = usb_init();
        if (nusb >= 0) {
            int nk = 0, nm = 0, ns = 0;
            for (int i = 0; usb_device(i); i++) { int kd = usb_device(i)->kind; nk += kd == USB_KBD; nm += kd == USB_MOUSE; ns += kd == USB_MSC; }
            ksnprintf(m, sizeof m, "USB (xHCI): %d cihaz (klavye %d, fare %d, depolama %d)", usb_device_count(), nk, nm, ns);
            log_ok(m);
        } else {
            console_set_color(C_GRAY, C_BG); kprintf("[ -- ] USB denetleyicisi (xHCI) bulunamadi\n"); console_set_color(C_FG, C_BG);
        }
    }

    UINT64 pages[16];
    UINT64 regions = mem_collect(pages);
    kprintf("\nBellek: %llu MB kullanilabilir (%llu bolge)\n", usable_mb(pages), regions);
    kprintf("'help' yazarak komutlari gorebilirsiniz.\n\n");

#ifdef BOOT_CRASH
    {   // CI: kasitli hata denemesi (ornek: BOOT_CRASH=smap). Panik beklenir; seri porta yazilir.
        serial_puts("\n[CRASHTEST] basliyor: " BOOT_CRASH "\n");
        cmd_crash(BOOT_CRASH);
        serial_puts("\n[CRASHTEST] PANIK OLMADI\n");
        acpi_poweroff();
    }
#endif

#if BOOT_SELFTEST
    {   // CI: tum testleri calistir, sonucu seri porta yaz, makineyi kapat
        int ok = selftest_all(bi, 1);      // CI diskleri bu is icin olusturulur: yazma testleri serbest
        serial_puts(ok ? "\n[SELFTEST] SONUC: GECTI\n" : "\n[SELFTEST] SONUC: KALDI\n");
        acpi_poweroff();
    }
#endif

#if START_DESKTOP
    console_set_color(C_CYAN, C_BG);
    kprintf("Masaustu baslatiliyor...\n");
    console_set_color(C_FG, C_BG);
    thread_sleep_ms(1200);                  // acilis mesajlari okunabilsin
    if (wm_start(bi)) {
        console_set_color(C_CYAN, C_BG);
        kprintf("DEOS " DEOS_VERSION " -- terminal\n");
        console_set_color(C_GRAY, C_BG);
        kprintf("'help' yazin. Fare pencereye girince yakalanir (Ctrl+Alt+G: birak).\n\n");
        console_set_color(C_FG, C_BG);
    } else {
        kprintf("Masaustu baslatilamadi, metin modunda devam ediliyor.\n");
    }
#endif
    g_len = 0;
    prompt();

    while (1) {
        if (!wm_active()) shell_tick();
        int c = console_getchar();          // masaustunde terminal penceresinden, metin modunda klavyeden
        if (c >= 0) { shell_key((char)c); g_cursor_state = -1; continue; }
        thread_sleep_ms(10);          // diger thread'lere / idle'a ver; 10 ms sonra klavyeye tekrar bak
    }
}

// DEOS -- "gercek donanim hazirligi" otomatik testleri + tum testleri sirayla kosturan toplayici.
// Kabukta: 'hwtest' (bu dosyadaki testler), 'selftest' (hepsi). BOOT_SELFTEST=1 ile derlenirse
// acilista hepsi calisir, sonuc seri porta "[SELFTEST] SONUC: GECTI/KALDI" olarak yazilir (CI).
#include "types.h"
#include "config.h"
#include "selftest.h"
#include "kprintf.h"
#include "console.h"
#include "serial.h"
#include "string.h"
#include "heap.h"
#include "pmm.h"
#include "paging.h"
#include "acpi.h"
#include "iommu.h"
#include "keyboard.h"
#include "usb.h"
#include "net.h"
#include "net_priv.h"
#include "timer.h"
#include "thread.h"
#include "heap.h"
#include "blk.h"
#include "elf.h"
#include "syscall.h"
#include "process.h"
#include "syscall_abi.h"

#define C_OK   0x009ECE6Au
#define C_BAD  0x00F7768Eu
#define C_FG   0x00C0CAF5u
#define C_BG   0x001A1B26u

static int g_ok, g_fail_count;
static int g_allow_write;      // 1: diske yazan testler serbest (yalnizca CI/test diskleri!)

static void check(int cond, const char *name) {
    console_set_color(cond ? C_OK : C_BAD, C_BG);
    kprintf(cond ? "  [PASS] " : "  [FAIL] ");
    console_set_color(C_FG, C_BG);
    kprintf("%s\n", name);
    if (!cond) { g_ok = 0; g_fail_count++; }
}
static void note(const char *msg) { kprintf("  [ -- ] %s\n", msg); }
// Aygit yoksa: normalde "atlandi" notu; CI modunda (BOOT_SELFTEST=1, ci_test.sh aygitlari takar) BASARISIZLIK.
static void absent(const char *needed, const char *skip_msg) {
    if (BOOT_SELFTEST) check(0, needed); else note(skip_msg);
}
static void head(const char *t) { kprintf("\n-- %s --\n", t); }

// ------------------------------------------------------------------ ACPI / CPU
static void test_acpi_cpu(void) {
    head("ACPI / MADT / CPU");
    if (!acpi_present()) { absent("CI: ACPI bulunamadi", "ACPI yok: bu bolum atlandi"); return; }
    check(acpi_oem() && acpi_oem()[0] != 0, "ACPI OEM kimligi okundu");
    UINT32 flen = 0; UINT64 fa = acpi_find_table("FACP", &flen);
    check(fa != 0 && flen >= 116, "acpi_find_table: FADT bulundu, uzunluk ve saglama dogru");
    check(acpi_find_table("ZZZZ", 0) == 0, "acpi_find_table: olmayan imza 0 doner");
    check(!acpi_madt() || acpi_find_table("APIC", 0) == acpi_madt(), "acpi_find_table: MADT, acpi_madt() ile ayni");
    if (!acpi_madt()) { absent("CI: MADT bulunamadi", "MADT yok: APIC bilgisi atlandi"); return; }
    check(acpi_cpu_count() >= 1, "MADT: en az 1 etkin islemci");
    check(acpi_lapic_addr() != 0 && (acpi_lapic_addr() & 0xFFF) == 0, "MADT: Local APIC adresi sayfa hizali ve sifir degil");
    int ios_ok = 1;
    for (int i = 0; i < acpi_ioapic_count(); i++) {
        UINT8 id; UINT32 addr, gsi;
        if (!acpi_ioapic_get(i, &id, &addr, &gsi) || addr == 0 || (addr & 0xFFF)) ios_ok = 0;
    }
    check(acpi_ioapic_count() == 0 || ios_ok, "MADT: I/O APIC adresleri gecerli");
    int iso_ok = 1;
    for (int i = 0; i < acpi_iso_count(); i++) {
        UINT8 src; UINT32 gsi; UINT16 fl;
        if (!acpi_iso_get(i, &src, &gsi, &fl) || src >= 16) iso_ok = 0;
    }
    check(iso_ok, "MADT: IRQ yonlendirme istisnalari ISA araliginda");
    UINT8 d; check(!acpi_ioapic_get(-1, &d, 0, 0) && !acpi_ioapic_get(acpi_ioapic_count(), &d, 0, 0), "I/O APIC indeks siniri denetimi");

    // CPUID ile islemci sayisi tutarliligi (SMP bilgisi: raporlanir, kullanilmaz)
    UINT32 a, b, c, dd;
    __asm__ __volatile__ ("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(dd) : "a"(1), "c"(0));
    check((dd & (1u << 9)) != 0, "CPUID: islemcide Local APIC var (APIC kullanimina hazir)");
    acpi_dump();                    // 'acpi' kabuk komutunun ciktisi (kod yolu da denenmis olur)
}

// ------------------------------------------------------------------ Koruma ozellikleri (SMEP/SMAP)
static void test_cpu_protect(void) {
    head("CPU koruma ozellikleri");
    UINT64 cr4; __asm__ __volatile__ ("mov %%cr4, %0" : "=r"(cr4));
    UINT32 a, b, c, d, maxleaf;
    __asm__ __volatile__ ("cpuid" : "=a"(maxleaf), "=b"(b), "=c"(c), "=d"(d) : "a"(0), "c"(0));
    int smep_hw = 0, smap_hw = 0;
    if (maxleaf >= 7) {
        __asm__ __volatile__ ("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(7), "c"(0));
        smep_hw = (b >> 7) & 1; smap_hw = (b >> 20) & 1;
    }
    int smep_on = (int)((cr4 >> 20) & 1), smap_on = (int)((cr4 >> 21) & 1);
    check(smep_on == (syscall_smep_enabled() ? 1 : 0), "SMEP: CR4.20 durumu syscall_smep_enabled() ile tutarli");
    check(!smep_hw || smep_on, "SMEP: islemci destekliyorsa acik");
    check((cr4 & (1ull << 5)) != 0, "CR4.PAE acik");
    kprintf("  bilgi: SMEP donanim=%d acik=%d | SMAP donanim=%d acik=%d (SMAP henuz uygulanmadi -- bilinen eksik)\n",
            smep_hw, smep_on, smap_hw, smap_on);
}

// ------------------------------------------------------------------ Klavye duzenleri
static void test_keymaps(void) {
    head("Klavye duzenleri (TR Q / US)");
    const int TR = KBD_LAYOUT_TR, US = KBD_LAYOUT_US;
    #define T(sc, sh, cp) ((UINT8)keyboard_translate(TR, (sc), (sh), (cp)))
    check(T(0x17, 0, 0) == 0x96 && T(0x17, 1, 0) == 'I', "TR: I tusu: ı (kucuk) / I (buyuk)");
    check(T(0x28, 0, 0) == 'i'  && T(0x28, 1, 0) == 0x97, "TR: i tusu: i (kucuk) / İ (buyuk)");
    check(T(0x17, 0, 1) == 'I'  && T(0x28, 0, 1) == 0x97, "TR: CapsLock: ı->I, i->İ");
    check(T(0x17, 1, 1) == 0x96 && T(0x28, 1, 1) == 'i', "TR: Shift+CapsLock tersler");
    check(T(0x1A, 0, 0) == 0x90 && T(0x1A, 1, 0) == 0x91, "TR: ğ / Ğ");
    check(T(0x1B, 0, 0) == 0x92 && T(0x1B, 1, 0) == 0x93, "TR: ü / Ü");
    check(T(0x27, 0, 0) == 0x94 && T(0x27, 1, 0) == 0x95, "TR: ş / Ş");
    check(T(0x33, 0, 0) == 0x98 && T(0x33, 1, 0) == 0x99, "TR: ö / Ö");
    check(T(0x34, 0, 0) == 0x9A && T(0x34, 1, 0) == 0x9B, "TR: ç / Ç");
    const char *shifted = "!'^+%&/()=";
    int num_ok = 1;
    for (int i = 0; i < 10; i++) {
        char plain = (char)((i == 9) ? '0' : ('1' + i));
        if (keyboard_translate(TR, (UINT8)(0x02 + i), 0, 0) != plain) num_ok = 0;
        if (keyboard_translate(TR, (UINT8)(0x02 + i), 1, 0) != shifted[i]) num_ok = 0;
        if (keyboard_translate(TR, (UINT8)(0x02 + i), 0, 1) != plain) num_ok = 0;     // Caps rakamlari etkilemez
    }
    check(num_ok, "TR: rakam satiri + Shift (!'^+%&/()=), Caps etkisiz");
    check(keyboard_translate(TR, 0x1E, 0, 0) == 'a' && keyboard_translate(TR, 0x1E, 1, 0) == 'A', "TR: a / A");
    check(keyboard_translate(TR, 0x39, 0, 0) == ' ' && keyboard_translate(TR, 0x1C, 0, 0) == '\n', "TR: bosluk ve Enter");
    check(keyboard_translate(US, 0x17, 0, 0) == 'i' && keyboard_translate(US, 0x17, 1, 0) == 'I', "US: i / I");
    check(keyboard_translate(US, 0x10, 0, 0) == 'q' && keyboard_translate(US, 0x10, 0, 1) == 'Q', "US: q ve CapsLock");
    check(keyboard_translate(US, 0x02, 1, 0) == '!' && keyboard_translate(US, 0x1A, 1, 0) == '{', "US: Shift+1 = !, Shift+[ = {");
    check(keyboard_translate(TR, 0x3B, 0, 0) == 0 && keyboard_translate(US, 0x70, 0, 0) == 0, "tablo disi tarama kodu 0 doner");
    // Turkce harfler: her biri ayri baytta, KEY_* kodlariyla (0x81-0x89) cakismaz
    int clash = 0;
    for (int sc = 0; sc < 0x3B; sc++) for (int sh = 0; sh < 2; sh++) {
        UINT8 ch = (UINT8)keyboard_translate(TR, (UINT8)sc, sh, 0);
        if (ch >= 0x81 && ch <= 0x89) clash = 1;
    }
    check(!clash, "TR: harf baytlari ozel tus kodlariyla (0x81-0x89) cakismiyor");
    #undef T

    // USB HID klavyesi ayni duzeni kullanmali
    int saved = keyboard_get_layout();
    while (keyboard_get_layout() != TR) keyboard_toggle_layout();
    check((UINT8)usb_hid_translate(0x0C, 0, 0) == 0x96 && usb_hid_translate(0x0C, 1, 0) == 'I', "USB HID (TR): ı / I");
    check(usb_hid_translate(0x34, 0, 0) == 'i' && (UINT8)usb_hid_translate(0x34, 1, 0) == 0x97, "USB HID (TR): i / İ");
    check((UINT8)usb_hid_translate(0x0C, 0, 1) == 'I' && (UINT8)usb_hid_translate(0x34, 0, 1) == 0x97, "USB HID (TR): CapsLock");
    check((UINT8)usb_hid_translate(0x33, 0, 0) == 0x94 && (UINT8)usb_hid_translate(0x2F, 1, 0) == 0x91, "USB HID (TR): ş ve Ğ");
    check(usb_hid_translate(0x04, 0, 0) == 'a' && usb_hid_translate(0x04, 1, 0) == 'A', "USB HID: a / A");
    while (keyboard_get_layout() != saved) keyboard_toggle_layout();
}

// ------------------------------------------------------------------ ELF yukleyici (bozuk girdiler)
typedef struct { UINT8 e_ident[16]; UINT16 e_type, e_machine; UINT32 e_version; UINT64 e_entry, e_phoff, e_shoff;
                 UINT32 e_flags; UINT16 e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx; } T_Ehdr;
typedef struct { UINT32 p_type, p_flags; UINT64 p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align; } T_Phdr;
#define ELF_SIZE 256

static void elf_good(UINT8 *img) {
    memset(img, 0, ELF_SIZE);
    T_Ehdr *e = (T_Ehdr *)img;
    e->e_ident[0] = 0x7F; e->e_ident[1] = 'E'; e->e_ident[2] = 'L'; e->e_ident[3] = 'F';
    e->e_ident[4] = 2; e->e_ident[5] = 1; e->e_ident[6] = 1;
    e->e_type = 2; e->e_machine = 62; e->e_version = 1;
    e->e_entry = USER_BASE + 0x1000; e->e_phoff = sizeof(T_Ehdr);
    e->e_ehsize = sizeof(T_Ehdr); e->e_phentsize = sizeof(T_Phdr); e->e_phnum = 1;
    T_Phdr *p = (T_Phdr *)(img + sizeof(T_Ehdr));
    p->p_type = 1; p->p_flags = 5 /*R+X*/; p->p_offset = 0x80; p->p_vaddr = USER_BASE + 0x1000;
    p->p_filesz = 16; p->p_memsz = 0x2000; p->p_align = 0x1000;
    for (int i = 0; i < 16; i++) img[0x80 + i] = (UINT8)(0xA0 + i);
}

static int elf_try(const UINT8 *img, UINT64 size, UINT64 *entry) {
    UINT64 cr3 = paging_new_user_space();
    if (!cr3) return -99;
    int r = elf_load(img, size, cr3, entry);
    paging_destroy_user_space(cr3);
    return r;
}

static void test_elf(void) {
    head("ELF yukleyici: gecerli ve bozuk girdiler");
    UINT8 *img = kmalloc(ELF_SIZE);
    if (!img) { check(0, "bellek"); return; }
    T_Ehdr *e = (T_Ehdr *)img; T_Phdr *p = (T_Phdr *)(img + sizeof(T_Ehdr));
    pmm_stats_t s0, s1; pmm_get_stats(&s0);
    UINT64 entry = 0;

    elf_good(img);
    check(elf_try(img, ELF_SIZE, &entry) == 0 && entry == USER_BASE + 0x1000, "gecerli ELF yuklendi, giris noktasi dogru");

    // Gecerli ELF'i gercek bir adres uzayinda ayrintili denetle
    {
        UINT64 cr3 = paging_new_user_space(), phys = 0, fl = 0; UINT32 sh = 0;
        int ok = cr3 && elf_load(img, ELF_SIZE, cr3, &entry) == 0;
        check(ok, "yeni adres uzayina yukleme");
        if (ok) {
            check(paging_query_in(cr3, USER_BASE + 0x1000, &phys, &fl, &sh) && (fl & PTE_U) && !(fl & PTE_W) && !(fl & PTE_NX),
                  "R+X segment: kullanici erisimli, yazilamaz, calistirilabilir");
            check(paging_query_in(cr3, USER_BASE + 0x2000, &phys, &fl, &sh), "memsz > filesz: ikinci sayfa (bss) eslendi");
            check(!paging_query_in(cr3, USER_BASE + 0x3000, &phys, &fl, &sh), "segment disindaki sayfa eslenmedi");
            paging_query_in(cr3, USER_BASE + 0x1000, &phys, &fl, &sh);
            const UINT8 *pg = (const UINT8 *)(UINTN)phys;
            int data_ok = 1; for (int i = 0; i < 16; i++) if (pg[i] != (UINT8)(0xA0 + i)) data_ok = 0;
            for (int i = 16; i < 4096; i++) if (pg[i] != 0) data_ok = 0;
            check(data_ok, "dosya baytlari kopyalandi, kalani sifirlandi");
        }
        if (cr3) paging_destroy_user_space(cr3);
    }
    // yazilabilir veri segmenti: NX + W
    elf_good(img); p->p_flags = 6; p->p_vaddr = USER_BASE + 0x1000;
    {
        UINT64 cr3 = paging_new_user_space(), phys, fl; UINT32 sh;
        int ok = cr3 && elf_load(img, ELF_SIZE, cr3, &entry) == 0 && paging_query_in(cr3, USER_BASE + 0x1000, &phys, &fl, &sh);
        check(ok && (fl & PTE_W) && (fl & PTE_NX), "R+W segment: yazilabilir ve calistirilamaz (NX)");
        if (cr3) paging_destroy_user_space(cr3);
    }

    #define BAD(desc, mut) do { elf_good(img); mut; check(elf_try(img, ELF_SIZE, &entry) == -1, desc); } while (0)
    BAD("reddedildi: bozuk imza (magic)",             e->e_ident[1] = 'X');
    BAD("reddedildi: 32 bit ELF",                     e->e_ident[4] = 1);
    BAD("reddedildi: big-endian",                     e->e_ident[5] = 2);
    BAD("reddedildi: ET_DYN (tur 3)",                 e->e_type = 3);
    BAD("reddedildi: x86-64 disi makine",             e->e_machine = 40);
    BAD("reddedildi: phnum = 0",                      e->e_phnum = 0);
    BAD("reddedildi: phnum > 16",                     e->e_phnum = 17);
    BAD("reddedildi: yanlis phentsize",               e->e_phentsize = 32);
    BAD("reddedildi: phoff dosya disinda",            e->e_phoff = ELF_SIZE + 1);
    BAD("reddedildi: phoff + tablo dosya disina tasiyor", e->e_phoff = ELF_SIZE - 8);
    BAD("reddedildi: phoff = 2^64-1 (tasma)",         e->e_phoff = ~0ULL);
    BAD("reddedildi: giris noktasi kullanici alani altinda", e->e_entry = 0x1000);
    BAD("reddedildi: giris noktasi kernel adresinde", e->e_entry = 0xFFFF800000000000ULL);
    BAD("reddedildi: giris noktasi heap bolgesinde",  e->e_entry = USER_HEAP_BASE);
    BAD("reddedildi: filesz > memsz",                 p->p_filesz = 0x3000);
    BAD("reddedildi: offset dosya disinda",           p->p_offset = ELF_SIZE + 1);
    BAD("reddedildi: offset + filesz dosya disina tasiyor", p->p_offset = 0xF8; p->p_filesz = 16);
    BAD("reddedildi: offset = 2^64-1 (tasma)",        p->p_offset = ~0ULL);
    BAD("reddedildi: vaddr + memsz tasiyor",          p->p_vaddr = ~0ULL - 100; p->p_memsz = 4096; p->p_filesz = 0);
    BAD("reddedildi: segment kernel adres uzayinda",  p->p_vaddr = 0x1000);
    BAD("reddedildi: segment sifir sayfasinda (NULL)", p->p_vaddr = USER_BASE);
    BAD("reddedildi: segment heap bolgesine tasiyor", p->p_vaddr = USER_HEAP_BASE - 0x1000);
    BAD("reddedildi: PT_LOAD yok (sadece PT_NOTE)",   p->p_type = 4);
    BAD("reddedildi: memsz = 0 segment",              p->p_memsz = 0);
    #undef BAD
    check(elf_try(img, sizeof(T_Ehdr) - 1, &entry) == -1, "reddedildi: baslik kesik (boyut < 64)");
    check(elf_try(img, 0, &entry) == -1, "reddedildi: bos goruntu");

    // Cakisan iki segment: ayni sayfaya ikinci esleme reddedilir, sizinti olmaz
    elf_good(img); e->e_phnum = 2;
    { T_Phdr *p2 = p + 1; *p2 = *p; }
    check(elf_try(img, ELF_SIZE, &entry) == -1, "reddedildi: cakisan segmentler");

    kfree(img);
    pmm_get_stats(&s1);
    check(s1.free_pages == s0.free_pages, "hicbir ELF denemesi (basarili/basarisiz) sayfa sizdirmadi");
}

// ------------------------------------------------------------------ Ag
static UINT32 mk_udp_frame(UINT8 *f, UINT32 src, UINT32 dst, UINT16 sp, UINT16 dp, const void *pl, UINT32 plen) {
    net_status_t st; net_get_status(&st);
    memcpy(f, st.mac, 6);
    memset(f + 6, 0x02, 6); f[11] = 0x99;
    put16(f + 12, 0x0800);
    UINT8 *ip = f + 14;
    ip[0] = 0x45; ip[1] = 0; put16(ip + 2, (UINT16)(28 + plen)); put16(ip + 4, 1); put16(ip + 6, 0);
    ip[8] = 64; ip[9] = 17; ip[10] = ip[11] = 0;
    memcpy(ip + 12, &src, 4); memcpy(ip + 16, &dst, 4);
    put16(ip + 10, net_csum16(ip, 20));
    UINT8 *u = ip + 20;
    put16(u, sp); put16(u + 2, dp); put16(u + 4, (UINT16)(8 + plen)); u[6] = u[7] = 0;
    if (plen) memcpy(u + 8, pl, plen);
    UINT16 c = l4_csum(src, dst, 17, u, 8 + plen);
    put16(u + 6, c ? c : 0xFFFF);
    return 14 + 28 + plen;
}
static void fix_ip_csum(UINT8 *f) { f[24] = f[25] = 0; put16(f + 24, net_csum16(f + 14, 20)); }

static UINT32 mk_dhcp(UINT8 *p, UINT32 xid, int type) {
    memset(p, 0, 320);
    p[0] = 2; p[1] = 1; p[2] = 6; put32(p + 4, xid);
    p[16] = 10; p[17] = 0; p[18] = 2; p[19] = 15;
    memcpy(p + 28, net_mac(), 6); put32(p + 236, 0x63825363);
    UINT32 o = 240;
    p[o++] = 53; p[o++] = 1; p[o++] = (UINT8)type;
    p[o++] = 1; p[o++] = 4; p[o++] = 255; p[o++] = 255; p[o++] = 255; p[o++] = 0;
    p[o++] = 3; p[o++] = 4; p[o++] = 10; p[o++] = 0; p[o++] = 2; p[o++] = 2;
    p[o++] = 6; p[o++] = 8; p[o++] = 10; p[o++] = 0; p[o++] = 2; p[o++] = 3; p[o++] = 8; p[o++] = 8; p[o++] = 8; p[o++] = 8;
    p[o++] = 51; p[o++] = 4; put32(p + o, 86400); o += 4;
    p[o++] = 54; p[o++] = 4; p[o++] = 10; p[o++] = 0; p[o++] = 2; p[o++] = 2;
    p[o++] = 255;
    return o;
}

static int wait_dhcp(UINT32 ms) {
    UINT64 start = timer_get_ticks();
    while (timer_get_ticks() - start < (UINT64)ms * TIMER_HZ / 1000) {
        int st = dhcp_state();
        if (st != DHCP_DISCOVER && st != DHCP_REQUEST) return st;
        thread_sleep_ms(50);
    }
    return dhcp_state();
}

static void test_net_units(void) {
    head("Ag: yardimcilar, DHCP/DNS ayristirici, IPv4/UDP dogrulama");
    // RFC 1071 ornegi: 00 01 f2 03 f4 f5 f6 f7 -> toplam 0xddf2, saglama 0x220d
    const UINT8 ex[8] = { 0x00, 0x01, 0xf2, 0x03, 0xf4, 0xf5, 0xf6, 0xf7 };
    check(net_csum16(ex, 8) == 0x220D, "saglama toplami: RFC 1071 ornegi");
    UINT8 withsum[10]; memcpy(withsum, ex, 8); withsum[8] = 0x22; withsum[9] = 0x0D;
    check(net_csum16(withsum, 10) == 0, "saglama toplami: dogru toplamli veri 0 verir");
    const UINT8 odd[1] = { 0x01 };
    check(net_csum16(odd, 1) == 0xFEFF, "saglama toplami: tek bayt (tek uzunluk dolgusu)");
    check(net_csum16(ex, 0) == 0xFFFF, "saglama toplami: bos veri");

    UINT32 ip = 0;
    check(net_parse_ip("10.0.2.15", &ip) == 0 && ip == IP4(10, 0, 2, 15), "IP ayristirma: 10.0.2.15");
    check(net_parse_ip("255.255.255.255", &ip) == 0 && ip == 0xFFFFFFFFu, "IP ayristirma: 255.255.255.255");
    check(net_parse_ip("0.0.0.0", &ip) == 0 && ip == 0, "IP ayristirma: 0.0.0.0");
    check(net_parse_ip("192.168.1.10 ", &ip) == 0, "IP ayristirma: sondaki bosluk kabul");
    check(net_parse_ip("256.1.1.1", &ip) != 0, "IP reddedildi: 256");
    check(net_parse_ip("1.2.3", &ip) != 0, "IP reddedildi: 3 parca");
    check(net_parse_ip("1.2.3.4.5", &ip) != 0, "IP reddedildi: 5 parca");
    check(net_parse_ip("1.2.3.4x", &ip) != 0, "IP reddedildi: sondaki cop karakter");
    check(net_parse_ip("1..2.3", &ip) != 0, "IP reddedildi: bos parca");
    check(net_parse_ip("", &ip) != 0, "IP reddedildi: bos dizge");
    check(net_parse_ip("1.2.3.4444", &ip) != 0, "IP reddedildi: 4 haneli parca");
    check(net_parse_ip("a.b.c.d", &ip) != 0, "IP reddedildi: rakam disi");
    char sb[16]; net_format_ip(IP4(192, 168, 1, 77), sb);
    check(!strcmp(sb, "192.168.1.77"), "net_format_ip: dogru bayt sirasi");

    // ---- DHCP ayristirici ----
    UINT8 d[320]; dhcp_msg_t m; UINT32 n = mk_dhcp(d, 0xCAFEBABE, 2);
    check(dhcp_parse_reply(d, n, 0xCAFEBABE, &m) == 0 && m.type == 2 && m.yiaddr == IP4(10, 0, 2, 15) &&
          m.mask == IP4(255, 255, 255, 0) && m.gw == IP4(10, 0, 2, 2) && m.dns == IP4(10, 0, 2, 3) &&
          m.server == IP4(10, 0, 2, 2) && m.lease == 86400, "DHCP: gecerli Offer dogru ayristirildi (ilk DNS alinir)");
    check(dhcp_parse_reply(d, n, 0x11111111, &m) != 0, "DHCP reddedildi: yanlis islem kimligi (xid)");
    n = mk_dhcp(d, 1, 5); d[28] ^= 0xFF;
    check(dhcp_parse_reply(d, n, 1, &m) != 0, "DHCP reddedildi: baska istemcinin MAC'i");
    n = mk_dhcp(d, 1, 5); d[236] = 0;
    check(dhcp_parse_reply(d, n, 1, &m) != 0, "DHCP reddedildi: bozuk magic cookie");
    n = mk_dhcp(d, 1, 5); d[0] = 1;
    check(dhcp_parse_reply(d, n, 1, &m) != 0, "DHCP reddedildi: BOOTREQUEST");
    check(dhcp_parse_reply(d, 100, 1, &m) != 0, "DHCP reddedildi: kisa paket");
    n = mk_dhcp(d, 1, 5); d[243] = 1; d[244] = 4; d[245] = 200;   // 'mask' secenegi uzunlugu paket disina tasiyor
    d[243] = 6; d[244] = 250;                                       // DNS secenegi 250 bayt iddia ediyor
    check(dhcp_parse_reply(d, n, 1, &m) != 0, "DHCP reddedildi: secenek uzunlugu paket sinirini asiyor");
    n = mk_dhcp(d, 1, 6);
    check(dhcp_parse_reply(d, n, 1, &m) == 0 && m.type == 6, "DHCP: NAK turu taninir");
    n = mk_dhcp(d, 1, 5); d[240] = 255;                             // 'end' hemen: ileti turu yok
    check(dhcp_parse_reply(d, n, 1, &m) != 0, "DHCP reddedildi: ileti turu (53) yok");

    // ---- DNS ----
    UINT8 q[300]; int ql = dns_build_query(0x1234, "example.com", q, sizeof q);
    static const UINT8 want[] = { 0x12, 0x34, 0x01, 0x00, 0, 1, 0, 0, 0, 0, 0, 0, 7, 'e', 'x', 'a', 'm', 'p', 'l', 'e', 3, 'c', 'o', 'm', 0, 0, 1, 0, 1 };
    check(ql == (int)sizeof want && !memcmp(q, want, sizeof want), "DNS: 'example.com' sorgusu birebir dogru");
    check(dns_build_query(1, "a.b.", q, sizeof q) > 0, "DNS: sondaki nokta kabul");
    check(dns_build_query(1, "", q, sizeof q) < 0 && dns_build_query(1, "a..b", q, sizeof q) < 0 && dns_build_query(1, ".a", q, sizeof q) < 0,
          "DNS reddedildi: bos ad / bos etiket");
    char longlabel[80]; memset(longlabel, 'a', 64); longlabel[64] = 0;
    check(dns_build_query(1, longlabel, q, sizeof q) < 0, "DNS reddedildi: 64 karakterlik etiket");
    check(dns_build_query(1, "bad name.com", q, sizeof q) < 0 && dns_build_query(1, "a/b.com", q, sizeof q) < 0, "DNS reddedildi: gecersiz karakter");
    check(dns_build_query(1, "example.com", q, 20) < 0, "DNS reddedildi: tampon kucuk");

    UINT8 r[200]; UINT32 ans;
    #define DNS_HDR(an, flags) do { memset(r, 0, sizeof r); put16(r, 0x1234); put16(r + 2, (flags)); put16(r + 4, 1); put16(r + 6, (an)); \
        memcpy(r + 12, want + 12, 17); } while (0)
    // gecerli yanit: soru + (isaretci adli) A kaydi
    DNS_HDR(1, 0x8180);
    UINT32 o = 29; put16(r + o, 0xC00C); put16(r + o + 2, 1); put16(r + o + 4, 1); put32(r + o + 6, 60); put16(r + o + 10, 4);
    r[o + 12] = 93; r[o + 13] = 184; r[o + 14] = 216; r[o + 15] = 34; UINT32 rl = o + 16;
    check(dns_parse_response(r, rl, 0x1234, &ans) == 0 && ans == IP4(93, 184, 216, 34), "DNS: A kaydi (sikistirma isaretcili) dogru okundu");
    check(dns_parse_response(r, rl, 0x9999, &ans) != 0, "DNS reddedildi: yanlis kimlik");
    UINT8 save = r[2]; r[2] = 0x01;
    check(dns_parse_response(r, rl, 0x1234, &ans) != 0, "DNS reddedildi: QR=0 (yanit degil)");
    r[2] = save; r[3] = 0x83;
    check(dns_parse_response(r, rl, 0x1234, &ans) == -2, "DNS: NXDOMAIN taninir");
    r[3] = 0x82;
    check(dns_parse_response(r, rl, 0x1234, &ans) == -1, "DNS reddedildi: SERVFAIL");
    r[3] = 0x80;
    check(dns_parse_response(r, rl - 3, 0x1234, &ans) != 0, "DNS reddedildi: A kaydi verisi kesik");
    check(dns_parse_response(r, 11, 0x1234, &ans) != 0, "DNS reddedildi: baslik kesik");
    check(dns_parse_response(r, 20, 0x1234, &ans) != 0, "DNS reddedildi: soru bolumu kesik");
    put16(r + 6, 60); put16(r + 29 + 2, 28);                          // AAAA + "60 yanit" iddiasi, veri yok
    check(dns_parse_response(r, rl, 0x1234, &ans) != 0, "DNS reddedildi: yanit sayisi gercekten fazla (60 iddia)");
    put16(r + 6, 1); put16(r + 29 + 2, 1);
    r[12] = 63;                                                         // soru adinin ilk etiketi paket sonunu asiyor
    check(dns_parse_response(r, 40, 0x1234, &ans) != 0, "DNS reddedildi: etiket uzunlugu paket disina tasiyor");
    // CNAME + A: CNAME atlanip A bulunmali
    DNS_HDR(2, 0x8180);
    o = 29; put16(r + o, 0xC00C); put16(r + o + 2, 5); put16(r + o + 4, 1); put32(r + o + 6, 60); put16(r + o + 10, 2); put16(r + o + 12, 0xC00C);
    o += 14; put16(r + o, 0xC00C); put16(r + o + 2, 1); put16(r + o + 4, 1); put32(r + o + 6, 60); put16(r + o + 10, 4);
    r[o + 12] = 1; r[o + 13] = 2; r[o + 14] = 3; r[o + 15] = 4;
    check(dns_parse_response(r, o + 16, 0x1234, &ans) == 0 && ans == IP4(1, 2, 3, 4), "DNS: CNAME atlanip A kaydi bulundu");
    DNS_HDR(1, 0x8180);
    o = 29; put16(r + o, 0xC00C); put16(r + o + 2, 28); put16(r + o + 4, 1); put32(r + o + 6, 60); put16(r + o + 10, 16);
    check(dns_parse_response(r, o + 12 + 16, 0x1234, &ans) == -3, "DNS: yalnizca AAAA var -> A kaydi yok");
    // isaretci dongusu: ad kendini gosteriyor (takip edilmedigi icin sonsuz dongu yok)
    DNS_HDR(1, 0x8180); o = 29; put16(r + o, 0xC000 | o); put16(r + o + 2, 1); put16(r + o + 4, 1); put16(r + o + 10, 4);
    r[o + 12] = 5; r[o + 13] = 6; r[o + 14] = 7; r[o + 15] = 8;
    check(dns_parse_response(r, o + 16, 0x1234, &ans) == 0 && ans == IP4(5, 6, 7, 8), "DNS: kendini gosteren isaretci dongu yaratmaz");
    UINT32 resolved = 0;
    check(dns_resolve("10.9.8.7", &resolved, 100) == 0 && resolved == IP4(10, 9, 8, 7), "dns_resolve: 'A.B.C.D' dogrudan ayristirilir");
    #undef DNS_HDR

    // ---- IPv4 / UDP giris dogrulamasi (net_rx_inject ile hazirlanmis cerceveler) ----
    int s = udp_open(5300);
    check(s >= 0, "UDP: 5300 numarali soket acildi");
    check(udp_open(5300) < 0, "UDP: ayni port ikinci kez acilamaz");
    { int se = udp_open(0); check(se >= 0 && udp_local_port(se) >= 49152, "UDP: gecici port 49152+ araligindan atandi"); udp_close(se); }
    UINT8 f[1600]; UINT32 fl; UINT8 buf[64]; UINT32 fip; UINT16 fp;
    UINT32 me = net_ip(), peer = IP4(10, 0, 2, 2);
    const char *hello = "merhaba";
    fl = mk_udp_frame(f, peer, me, 777, 5300, hello, 7);
    net_rx_inject(f, fl);
    int rn = udp_recvfrom(s, buf, sizeof buf, &fip, &fp, 50);
    check(rn == 7 && !memcmp(buf, hello, 7) && fip == peer && fp == 777, "UDP: gecerli datagram teslim edildi (yukleme, gonderen IP/port)");
    // Ethernet dolgusu: cerceve IP toplam uzunlugundan uzun
    fl = mk_udp_frame(f, peer, me, 777, 5300, hello, 3); memset(f + fl, 0xEE, 20);
    net_rx_inject(f, fl + 20);
    rn = udp_recvfrom(s, buf, sizeof buf, 0, 0, 50);
    check(rn == 3 && !memcmp(buf, "mer", 3), "UDP: Ethernet dolgusu yukleme uzunlugunu etkilemez");
    // bozuk cerceveler: hicbiri teslim edilmemeli
    int bad_ok = 1;
    #define EXPECT_DROP(desc, setup) do { fl = mk_udp_frame(f, peer, me, 777, 5300, hello, 7); setup; net_rx_inject(f, fl); \
        if (udp_recvfrom(s, buf, sizeof buf, 0, 0, 20) >= 0) { bad_ok = 0; kprintf("    teslim edildi (olmamaliydi): %s\n", desc); } } while (0)
    EXPECT_DROP("ip basligi saglamasi", f[24] ^= 0x55);
    EXPECT_DROP("ip toplam uzunluk > cerceve", { put16(f + 16, 400); fix_ip_csum(f); });
    EXPECT_DROP("ip toplam uzunluk < baslik", { put16(f + 16, 10); fix_ip_csum(f); });
    EXPECT_DROP("udp uzunlugu > ip yuku", { put16(f + 14 + 20 + 4, 200); });
    EXPECT_DROP("udp uzunlugu < 8", { put16(f + 14 + 20 + 4, 5); });
    EXPECT_DROP("udp saglamasi yanlis", f[14 + 20 + 6] ^= 0x01);
    EXPECT_DROP("parcalanmis (MF)", { put16(f + 20, 0x2000); fix_ip_csum(f); });
    EXPECT_DROP("parca ofseti != 0", { put16(f + 20, 0x0010); fix_ip_csum(f); });
    EXPECT_DROP("hedef IP bize ait degil", { UINT32 other = IP4(10, 0, 2, 99); memcpy(f + 30, &other, 4); fix_ip_csum(f); });
    EXPECT_DROP("surum 6", { f[14] = 0x65; fix_ip_csum(f); });
    EXPECT_DROP("IHL = 4", { f[14] = 0x44; fix_ip_csum(f); });
    EXPECT_DROP("IHL cerceveden buyuk", { f[14] = 0x4F; fix_ip_csum(f); });
    EXPECT_DROP("baska hedef portu", put16(f + 14 + 20 + 2, 5301));
    #undef EXPECT_DROP
    check(bad_ok, "UDP/IPv4: 13 bozuk cerceve turunun hicbiri teslim edilmedi");
    fl = mk_udp_frame(f, peer, me, 777, 5300, hello, 7); f[14 + 20 + 6] = f[14 + 20 + 7] = 0;
    net_rx_inject(f, fl);
    check(udp_recvfrom(s, buf, sizeof buf, 0, 0, 50) == 7, "UDP: saglama alani 0 ('yok') kabul edilir");
    fl = mk_udp_frame(f, peer, IP4(255, 255, 255, 255), 777, 5300, hello, 7);
    net_rx_inject(f, fl);
    check(udp_recvfrom(s, buf, sizeof buf, 0, 0, 50) == 7, "UDP: yayin (255.255.255.255) datagrami teslim edilir");
    net_rx_inject(f, 5); net_rx_inject(f, 14 + 10); net_rx_inject(f, 14 + 19);
    check(1, "cok kisa cerceveler (5, 24, 33 bayt) cokme yaratmadi");
    // kuyruk tasmasi: 10 gonder, 8 kuyruga girer
    for (int i = 0; i < 10; i++) { fl = mk_udp_frame(f, peer, me, 777, 5300, hello, 7); net_rx_inject(f, fl); }
    int got = 0; while (udp_recvfrom(s, buf, sizeof buf, 0, 0, 10) >= 0) got++;
    check(got == 8 && udp_dropped(s) == 2, "UDP: kuyruk 8 datagramla sinirli, fazlasi sayilarak atilir");
    // connect filtresi
    check(udp_connect(s, peer, 777) == 0, "UDP: connect");
    fl = mk_udp_frame(f, IP4(10, 0, 2, 50), me, 777, 5300, hello, 7); net_rx_inject(f, fl);
    fl = mk_udp_frame(f, peer, me, 778, 5300, hello, 7); net_rx_inject(f, fl);
    check(udp_recvfrom(s, buf, sizeof buf, 0, 0, 30) < 0, "UDP: baglanilan es disindan gelenler (IP ya da port) atilir");
    fl = mk_udp_frame(f, peer, me, 777, 5300, hello, 7); net_rx_inject(f, fl);
    check(udp_recvfrom(s, buf, sizeof buf, 0, 0, 30) == 7, "UDP: baglanilan esten gelen kabul edilir");
    udp_close(s);
    check(udp_recvfrom(s, buf, sizeof buf, 0, 0, 10) == -2, "UDP: kapatilan soketten okuma -2 doner");
    check(udp_sendto(s, peer, 1, hello, 7) < 0, "UDP: kapatilan soketten gonderim reddedilir");
    int s2 = udp_open(5300);
    check(s2 >= 0, "UDP: kapatilan port yeniden acilabilir");
    check(udp_sendto(s2, peer, 9, hello, UDP_MAX_PAYLOAD + 1) < 0, "UDP: 1473 bayt yuk reddedilir");
    udp_close(s2);

    // TCP: tanimsiz baglantiya gelen/bozuk segmentler cokme ya da baglanti olusturma yaratmamali
    UINT8 tf[14 + 20 + 20]; memset(tf, 0, sizeof tf);
    { net_status_t st; net_get_status(&st); memcpy(tf, st.mac, 6); }
    put16(tf + 12, 0x0800); tf[14] = 0x45; put16(tf + 16, 40); tf[22] = 64; tf[23] = 6;
    memcpy(tf + 26, &peer, 4); memcpy(tf + 30, &me, 4); put16(tf + 24, net_csum16(tf + 14, 20));
    put16(tf + 34, 80); put16(tf + 36, 55555); tf[46] = 5 << 4; tf[47] = 0x02;
    put16(tf + 50, l4_csum(peer, me, 6, tf + 34, 20));
    int before = tcp_conn_count();
    net_rx_inject(tf, sizeof tf);
    tf[50] ^= 0xFF; net_rx_inject(tf, sizeof tf);
    tf[46] = 2 << 4; net_rx_inject(tf, sizeof tf);
    check(tcp_conn_count() == before, "TCP: tanimsiz/bozuk segmentler baglanti olusturmaz");
}

static int g_echo_ok;                       // yanki sunucusu (ci/echo_server.py) yanit verdi mi

#ifndef CI_ECHO_PORT
#define CI_ECHO_PORT 47007
#endif

static void test_net_live(void) {
    head("Ag (canli): DHCP, ICMP, UDP, TCP, DNS");
    if (!net_present()) { absent("CI: e1000 NIC bulunamadi", "e1000 yok: canli ag testleri atlandi (QEMU: -nic user,model=e1000)"); return; }
    net_status_t st;
    int ds = wait_dhcp(20000);
    net_get_status(&st);
    check(ds == DHCP_BOUND, "DHCP: acilista arka planda adres alindi (BOUND)");
    check(st.ip == IP4(10, 0, 2, 15) && st.mask == IP4(255, 255, 255, 0), "DHCP: IP 10.0.2.15, maske 255.255.255.0");
    check(st.gw == IP4(10, 0, 2, 2) && st.dns == IP4(10, 0, 2, 3) && st.server == IP4(10, 0, 2, 2), "DHCP: ag gecidi, DNS ve sunucu dogru");
    check(st.lease_s > 0, "DHCP: kira suresi alindi");
    check(st.link_up, "baglanti UP");
    char a[16]; net_format_ip(st.ip, a);
    check(!strcmp(a, "10.0.2.15"), "IP bellekte dogru bayt sirasiyla (10.0.2.15)");

    // ICMP: ilk TX halkasi turunu asan sayida ping (TX tanimlayici dondurme + asenkron geri alma)
    int ok = 0;
    for (int i = 0; i < 24; i++) if (net_ping_ip(st.gw, 1500) >= 0) ok++;
    check(ok == 24, "ping: 24/24 yanit (TX halkasi 16 tanimlayicidan fazla tur dondu)");
    check(net_ping_ip(IP4(10, 0, 2, 2), 1500) >= 0, "ping: QEMU slirp agi gecidi (10.0.2.2) yanit verdi");
    check(net_ping_ip(IP4(10, 0, 2, 77), 600) < 0, "ping: olmayan yerel adres zaman asimina duser (ARP yanit yok)");
    net_stats_t ns; net_get_stats(&ns);
    check(ns.tx_frames > 24 && ns.rx_frames > 24, "istatistikler: gonderilen/alinan cerceve sayaclari artiyor");

    // DHCP kira yenileme ve yeniden edinme
    dhcp_force_renew();
    net_get_status(&st);
    check(dhcp_state() == DHCP_BOUND && st.ip == IP4(10, 0, 2, 15), "DHCP: kira yenileme (unicast Request) sonrasi BOUND, IP ayni");
    check(dhcp_acquire(10000) == 0 && dhcp_state() == DHCP_BOUND, "DHCP: tekrar edinme (Discover->Offer->Request->Ack) basarili");
    net_get_status(&st);
    check(st.ip == IP4(10, 0, 2, 15) && st.gw == IP4(10, 0, 2, 2), "DHCP: tekrar edinmeden sonra yapilandirma ayni");

    // ---- yanki sunucusu (host: ci/echo_server.py, slirp uzerinden 10.0.2.2:CI_ECHO_PORT) ----
    UINT32 srv = IP4(10, 0, 2, 2);
    int us = udp_open(0);
    UINT8 tx[1500], rx[1600];
    for (int i = 0; i < 1500; i++) tx[i] = (UINT8)(i * 7 + 3);
    int probe = -1;
    for (int t = 0; t < 4 && probe < 0; t++) {
        udp_sendto(us, srv, CI_ECHO_PORT, "ping", 4);
        probe = udp_recvfrom(us, rx, sizeof rx, 0, 0, 1500);
    }
    if (probe != 4) {
        udp_close(us);
        absent("CI: yanki sunucusu (ci/echo_server.py) yanit vermiyor", "yanki sunucusu yok: UDP/TCP uctan uca testleri atlandi (host'ta: python3 ci/echo_server.py 47007)");
        return;
    }
    g_echo_ok = 1;
    int udp_ok = 1;
    static const UINT32 sizes[] = { 1, 100, 512, 1472 };
    for (int k = 0; k < 4; k++) {
        UINT32 fi; UINT16 fpo;
        int sent = udp_sendto(us, srv, CI_ECHO_PORT, tx, sizes[k]);
        int got = sent == 0 ? udp_recvfrom(us, rx, sizeof rx, &fi, &fpo, 2000) : -9;
        if (got != (int)sizes[k] || memcmp(rx, tx, sizes[k]) || fi != srv || fpo != CI_ECHO_PORT) { udp_ok = 0; kprintf("    UDP %u bayt basarisiz (got=%d)\n", sizes[k], got); }
    }
    check(udp_ok, "UDP uctan uca: 1/100/512/1472 bayt yanki birebir ayni (NAT uzerinden)");
    udp_close(us);

    int base_conns = tcp_conn_count();
    int h = tcp_connect(srv, CI_ECHO_PORT, 5000);
    check(h >= 0, "TCP: baglanti kuruldu (SYN/SYN-ACK/ACK)");
    if (h >= 0) {
        check(tcp_state(h) == 2, "TCP: durum ESTABLISHED");
        int w = tcp_write(h, "selam tcp", 9, 2000);
        int rn = tcp_read(h, rx, sizeof rx, 3000);
        check(w == 9 && rn == 9 && !memcmp(rx, "selam tcp", 9), "TCP: kucuk veri yanki birebir ayni");
        // buyuk akis: 100000 bayt, ayni anda yaz + oku (pencere, parcalama, siralama, geri cekilme)
        enum { BIG = 100000 };
        UINT8 *big = kmalloc(BIG);
        UINT64 sum_tx = 0, sum_rx = 0; int stream_ok = big != 0;
        if (big) {
            for (UINT32 i = 0; i < BIG; i++) big[i] = (UINT8)((i * 131 + (i >> 8)) & 0xFF);
            UINT32 wr = 0, rd = 0; UINT64 t0 = timer_get_ticks();
            UINT8 *back = kmalloc(BIG);
            if (!back) stream_ok = 0;
            while (back && (wr < BIG || rd < BIG) && timer_get_ticks() - t0 < 30 * TIMER_HZ) {
                if (wr < BIG) { int n = tcp_write(h, big + wr, BIG - wr > 3000 ? 3000 : BIG - wr, 50); if (n > 0) wr += (UINT32)n; }
                int n = tcp_read(h, back + rd, BIG - rd, 10);
                if (n > 0) rd += (UINT32)n; else if (n == 0 || (n < 0 && n != TCP_ERR_TIMEOUT)) break;
            }
            stream_ok = back && wr == BIG && rd == BIG && !memcmp(big, back, BIG);
            for (UINT32 i = 0; i < BIG; i++) { sum_tx += big[i]; if (back && i < rd) sum_rx += back[i]; }
            kprintf("    akis: yazilan %u, okunan %u bayt, %llu ms\n", wr, rd, (timer_get_ticks() - t0) * 1000 / TIMER_HZ);
            kfree(back); kfree(big);
        }
        check(stream_ok && sum_tx == sum_rx, "TCP: 100000 bayt es zamanli yaz/oku, yanki birebir ayni");
        check(tcp_close(h) == 0, "TCP: kapatma baslatildi (FIN)");
        UINT64 t1 = timer_get_ticks();
        while (tcp_conn_count() > base_conns && timer_get_ticks() - t1 < 8 * TIMER_HZ) thread_sleep_ms(50);
        check(tcp_conn_count() == base_conns, "TCP: baglanti kapanisi tamamlandi, kaynaklar serbest birakildi");
        check(tcp_write(h, "x", 1, 100) < 0, "TCP: kapatilan eli kullanmak hata verir");
    }
    int hc = tcp_connect(srv, 1, 4000);
    check(hc < 0, "TCP: dinlenmeyen porta baglanti hata verir (RST ya da zaman asimi)");
    thread_sleep_ms(200);
    check(tcp_conn_count() == base_conns, "TCP: basarisiz baglanti kaynak birakmadi");
    check(tcp_connect(IP4(10, 0, 2, 66), 80, 1500) < 0, "TCP: yanitsiz adrese baglanti zaman asimina duser");

    // DNS: slirp kendi yerel DNS vekili. 'localhost' cevrimdisi da cozulebilir; yoksa yalniz bilgi verilir.
    // Yanki sunucusu ayni portta basit bir DNS yanitlayicisi da calistirir (ci/echo_server.py): gercek sorgu/yanit turu.
    UINT32 lh = 0;
    check(dns_resolve_via("ci.example", &lh, 3000, srv, CI_ECHO_PORT) == 0 && lh == IP4(192, 0, 2, 123), "DNS uctan uca: ad -> 192.0.2.123 (gercek UDP sorgu/yanit)");
    lh = 0;
    check(dns_resolve_via("alias.test", &lh, 3000, srv, CI_ECHO_PORT) == 0 && lh == IP4(192, 0, 2, 123), "DNS uctan uca: CNAME + A yaniti cozuldu");
    UINT64 tn = timer_get_ticks();
    check(dns_resolve_via("nx.invalid", &lh, 3000, srv, CI_ECHO_PORT) != 0 && timer_get_ticks() - tn < 2 * TIMER_HZ, "DNS uctan uca: NXDOMAIN hemen hata verir (yeniden deneme yok)");
    int dr = dns_resolve("localhost", &lh, 4000);
    kprintf("  bilgi: dns_resolve(localhost) = %s%s\n", dr == 0 ? "cozuldu " : "cozulemedi", dr == 0 ? (net_format_ip(lh, a), a) : "");
    check(dns_resolve("bu-ad-yok.invalid", &lh, 3000) != 0, "DNS: olmayan ad cozulemez (NXDOMAIN/zaman asimi)");
}

// ------------------------------------------------------------------ ring 3: libtest (libc/pipe/env/spawn) + nettest (soketler)
static int run_user(const char *path, int argc, char **argv, int *code_out) {
    int pid = process_spawn_file(path, argc, argv, "/", 0, 0);
    if (pid < 0) return pid;
    UINT64 t0 = timer_get_ticks();
    while (thread_alive(pid) && timer_get_ticks() - t0 < 180 * TIMER_HZ) thread_sleep_ms(20);
    if (thread_alive(pid)) { thread_kill(pid); return -99; }
    int code = -77;
    if (!thread_get_exit_code(pid, &code)) return -98;
    *code_out = code;
    return 0;
}

static void test_userland(void) {
    head("Ring 3: C kutuphanesi, pipe, ortam, spawn; soket API");
    if (!g_allow_write) { note("ring 3 testleri atlandi: kok dosya sistemine gecici dosya (/LIBTEST.TMP, /SHT.TMP) yazarlar; 'hwtest write' ile calistirin"); return; }
    int code = -1;
    char *av[] = { "libtest", 0 };
    int r = run_user("/bin/libtest.elf", 1, av, &code);
    check(r == 0 && code == 0, "libtest: malloc/stdio/printf/ortam/pipe/spawn(fdmap) testlerinin hepsi gecti");
    if (r != 0) kprintf("    (libtest baslatma/bekleme hatasi: %d)\n", r);

    char *as[] = { "smash", 0 };
    r = run_user("/bin/smash.elf", 1, as, &code);
    check(r == 0 && code == -1, "ring 3 yigin canary'si: tasan program -1 ile sonlandirildi, cekirdek ayakta");

    if (!net_present() || !g_echo_ok) {
        absent("CI: nettest icin ag/yanki sunucusu yok", "nettest atlandi (ag ya da yanki sunucusu yok)");
        return;
    }
    int base = tcp_conn_count();
    char portbuf[12]; ksnprintf(portbuf, sizeof portbuf, "%d", CI_ECHO_PORT);
    char *an[] = { "nettest", portbuf, 0 };
    r = run_user("/bin/nettest.elf", 2, an, &code);
    check(r == 0 && code == 0, "nettest: ring 3 soket API (UDP/TCP/hata yollari) hepsi gecti");
    if (r != 0) kprintf("    (nettest baslatma/bekleme hatasi: %d)\n", r);
    UINT64 t1 = timer_get_ticks();
    while (tcp_conn_count() > base && timer_get_ticks() - t1 < 10 * TIMER_HZ) thread_sleep_ms(50);
    check(tcp_conn_count() == base, "surec acik TCP soketiyle cikti: cekirdek baglantiyi kapatip kaynaklari serbest birakti");
}

static void test_net(void) {
    if (net_present()) wait_dhcp(20000);          // acilistaki DHCP bitmeden birim testleri calistirilmaz (kabul filtresi degisir)
    test_net_units();
    test_net_live();
    test_userland();
}

// ------------------------------------------------------------------ NVMe
static void test_nvme(void) {
    head("NVMe");
    blockdev_t *d = blk_find("nvme0");
    if (!d) { absent("CI: nvme0 blok aygiti bulunamadi", "NVMe yok: bu bolum atlandi (QEMU: -device nvme)"); return; }
    check(d->sectors > 1024, "nvme0: kapasite makul (> 1024 sektor)");
    UINT8 *a = kmalloc(512 * 140 + 64), *b = kmalloc(512 * 140 + 64), *orig = kmalloc(512 * 8);
    if (!a || !b || !orig) { check(0, "bellek"); kfree(a); kfree(b); kfree(orig); return; }
    UINT8 *ua = a + 3, *ub = b + 5;      // bilerek hizasiz tamponlar (PRP / bounce dogrulamasi)

    check(blk_read(d, 0, 1, ua) == 0, "sektor 0 okundu (hizasiz tampon)");
    check(blk_read(d, 0, 1, ub) == 0 && !memcmp(ua, ub, 512), "sektor 0 iki kez ayni");
    check(blk_read(d, d->sectors, 1, ua) != 0, "disk sinirinin otesi okuma reddedildi");
    check(blk_read(d, d->sectors - 1, 1, ua) == 0, "son sektor okundu");
    check(blk_read(d, 0, 0, ua) != 0, "sifir sektor istegi reddedildi");

    // Cok sektorlu okuma: 130 sektor (128'lik bounce siniri asilir) = tek tek okumayla ayni
    UINT64 base = d->sectors / 2;
    int multi_ok = blk_read(d, base, 130, ua) == 0;
    int seq_ok = 1;
    for (UINT32 i = 0; i < 130 && seq_ok; i++) {
        if (blk_read(d, base + i, 1, ub) != 0 || memcmp(ua + i * 512, ub, 512)) seq_ok = 0;
    }
    check(multi_ok && seq_ok, "130 sektorluk okuma, tek tek okumayla birebir ayni");

    if (!g_allow_write) {
        note("yazma testleri atlandi (gercek diskte GPT yedegi diskin SONUNDADIR); yalniz test diskinde: 'hwtest write'");
        kfree(a); kfree(b); kfree(orig); return;
    }
    // Yazma / geri okuma / geri yukleme (diskin sonundaki 8 sektor)
    UINT64 w = d->sectors - 8;
    int saved = blk_read(d, w, 8, orig) == 0;
    for (int i = 0; i < 512 * 8; i++) ua[i] = (UINT8)(i * 31 + (i >> 9) + 7);
    int wr = saved && blk_write(d, w, 8, ua) == 0 && blk_read(d, w, 8, ub) == 0 && !memcmp(ua, ub, 512 * 8);
    check(wr, "8 sektor yazildi ve (hizasiz tamponlarla) ayni veri geri okundu");
    int one = saved && blk_write(d, w + 3, 1, ua + 4096) == 0 && blk_read(d, w + 3, 1, ub) == 0 && !memcmp(ub, ua + 4096, 512);
    check(one, "tek sektor ust uste yazma dogru");
    int rest = saved && blk_write(d, w, 8, orig) == 0 && blk_read(d, w, 8, ub) == 0 && !memcmp(ub, orig, 512 * 8);
    check(rest, "orijinal veri geri yuklendi");
    kfree(a); kfree(b); kfree(orig);
}

// ------------------------------------------------------------------ USB hub
static void test_usb_hub(void) {
    head("USB hub (hub arkasindaki cihazlar)");
    usbdev_t *hub = 0, *d;
    UINT64 t0 = timer_get_ticks();
    int kbd_behind = 0, msc_behind = 0;
    while (timer_get_ticks() - t0 < 6 * TIMER_HZ) {                    // hub cocuklari birkac yuz ms sonra gorunur
        hub = 0; kbd_behind = msc_behind = 0;
        for (int i = 0; (d = usb_device(i)); i++) {
            if (d->is_hub && !d->parent) hub = d;
            if (d->parent && !d->removed) { if (d->kind == USB_KBD) kbd_behind++; if (d->kind == USB_MSC) msc_behind++; }
        }
        if (hub && kbd_behind && msc_behind) break;
        thread_sleep_ms(100);
    }
    if (!hub) { absent("CI: USB hub bulunamadi (QEMU: -device usb-hub,bus=xhci.0,port=4)", "USB hub yok: hub testleri atlandi"); return; }
    check(hub->kind == USB_HUB && hub->hub_nports >= 2, "hub tanindi, port sayisi okundu (>= 2)");
    int linked = 0;
    for (int i = 0; i < USB_HUB_PORTS; i++) if (hub->child[i] && hub->child[i]->parent == hub && hub->child[i]->parent_port == i + 1) linked++;
    check(linked >= 2, "hub portlarina bagli >= 2 cihaz, ust/alt baglantilari tutarli");
    check(kbd_behind >= 1, "hub arkasinda klavye numaralandirildi ve HID olarak baglandi");
    check(msc_behind >= 1, "hub arkasinda USB depolama numaralandirildi ve baglandi");
    int route_ok = 1, depth_ok = 1;
    for (int i = 0; (d = usb_device(i)); i++) if (d->parent) {
        if ((d->route & 0xF) != (UINT32)d->parent_port) route_ok = 0;
        if (d->depth != d->parent->depth + 1 || d->port != d->parent->port) depth_ok = 0;
    }
    check(route_ok && depth_ok, "route string / derinlik / kok port degerleri tutarli");
    blockdev_t *b1 = blk_find("usb1"), *b0 = blk_find("usb0");
    check(b0 && b1, "iki USB blok aygiti (usb0 kok portta, usb1 hub arkasinda) kayitli");
    if (b1) {
        UINT8 *sec = kmalloc(512);
        int ok = sec && blk_read(b1, 0, 1, sec) == 0 && sec[510] == 0x55 && sec[511] == 0xAA;
        int label = ok && (!memcmp(sec + 71, "DEOSHUB", 7) || !memcmp(sec + 43, "DEOSHUB", 7));
        check(ok, "hub arkasindaki diskten sektor 0 okundu (acilis imzasi 55AA)");
        check(label, "okunan disk gercekten usb2.img (hub arkasindaki 'DEOSHUB' etiketi)");
        if (sec) kfree(sec);
    }
    if (b0) {
        UINT8 *sec = kmalloc(512);
        int ok = sec && blk_read(b0, 0, 1, sec) == 0 && sec[510] == 0x55 && sec[511] == 0xAA && (!memcmp(sec + 71, "DEOSUSB", 7) || !memcmp(sec + 43, "DEOSUSB", 7));
        check(ok, "kok porttaki disk (usb0, 'DEOSUSB') hub'li topolojide de okunuyor");
        if (sec) kfree(sec);
    }
}

// ------------------------------------------------------------------ IOMMU
static void test_iommu(void) {
    head("IOMMU (DMAR / IVRS)");
    int r1 = iommu_detect(), r2 = iommu_detect();
    check(r1 == r2, "iommu_detect() tekrar cagrilinca ayni sonuc");
    check(iommu_name() && iommu_name()[0], "iommu_name() bos degil");
    check((r1 != 0) == (iommu_kind() != IOMMU_NONE), "iommu_kind() tespit sonucuyla tutarli");
    iommu_unit_t u;
    check(!iommu_unit_info(-1, &u) && !iommu_unit_info(iommu_unit_count(), &u), "birim indeks siniri denetimi");
    if (iommu_kind() == IOMMU_INTEL) {
        check(iommu_unit_count() >= 1, "VT-d: en az 1 DRHD birimi");
        check(iommu_host_width() >= 36 && iommu_host_width() <= 57, "VT-d: fiziksel adres genisligi makul");
        int ok = iommu_unit_count() >= 1;
        for (int i = 0; i < iommu_unit_count(); i++) {
            if (!iommu_unit_info(i, &u) || u.base == 0 || (u.base & 0xFFF) || !u.regs_ok || u.ver_major == 0 || u.cap == 0 || u.ecap == 0) ok = 0;
        }
        check(ok, "VT-d: birim tabani sayfa hizali; VER/CAP/ECAP yazmaclari okundu");
        int off = 1;
        for (int i = 0; i < iommu_unit_count(); i++) if (iommu_unit_info(i, &u) && u.translation_on) off = 0;
        check(off, "VT-d: DMA remapping KAPALI (firmware acik birakmamis; surucu DMA'si engellenmez)");
    } else if (iommu_kind() == IOMMU_NONE) {
        absent("CI: Intel IOMMU (ACPI DMAR) bulunamadi (QEMU: -device intel-iommu)", "IOMMU yok (DMAR/IVRS tablosu bulunamadi): VT-d testleri atlandi");
    }
    iommu_dump();      // 'iommu' kabuk komutunun ciktisi (kod yolu da denenmis olur)
}

// ------------------------------------------------------------------ disariya acilanlar
int selftest_hw(const char *arg) {
    g_ok = 1; g_fail_count = 0;
    g_allow_write = arg && !strcmp(arg, "write");
    test_acpi_cpu();
    test_cpu_protect();
    test_keymaps();
    test_elf();
    test_net();
    test_nvme();
    test_usb_hub();
    test_iommu();
    kprintf("\nSonuc (hwtest): %s\n", g_ok ? "TUM TESTLER GECTI" : "BAZI TESTLER BASARISIZ");
    return g_ok;
}

int selftest_all(BootInfo *bi, int allow_write) {
    if (net_present()) wait_dhcp(20000);       // acilistaki DHCP thread'i bitmeden thread/bellek sayim testleri baslamaz
    struct { const char *name; int ok; } r[10]; int n = 0, all = 1;
    #define RUN(nm, call) do { kprintf("\n==== %s ====\n", nm); r[n].name = nm; r[n].ok = (call) ? 1 : 0; if (!r[n].ok) all = 0; n++; } while (0)
    RUN("bellek",    selftest_memory());
    RUN("sayfalama", selftest_paging(bi));
    RUN("thread",    selftest_threads());
    RUN("kullanici", selftest_user());
    RUN("depolama",  selftest_storage(!allow_write ? "" : blk_find("nvme0") ? "write nvme0" : (blk_find("sd1") ? "write sd1" : "")));
    RUN("dosya sis.", selftest_fs());
    RUN("usb",       selftest_usb());
    RUN("donanim",   selftest_hw(allow_write ? "write" : ""));
    #undef RUN
    kprintf("\n==== OZET ====\n");
    for (int i = 0; i < n; i++) {
        kprintf("  %s %s\n", r[i].ok ? "[PASS]" : "[FAIL]", r[i].name);
    }
    kprintf("Genel sonuc: %s\n", all ? "TUM TESTLER GECTI" : "BAZI TESTLER BASARISIZ");
    return all;
}

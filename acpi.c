// DEOS -- ACPI: RSDP dogrulama, XSDT/RSDT gezintisi, FADT alanlari, S5 (soft off).
// Sadece I/O space PM1 kontrol bloklarini destekler. Sayfalama ilk 4 GB'i esledigi
// icin tablo adresleri dogrudan fiziksel == sanal okunur.
#include "acpi.h"
#include "types.h"
#include "io.h"
#include "string.h"
#include "kprintf.h"
#include "timer.h"
#include "thread.h"

static UINT64 g_rsdp, g_fadt, g_madt, g_facs, g_dsdt;
static UINT64 g_pm1a_evt, g_pm1b_evt, g_pm1a_cnt, g_pm1b_cnt;
static UINT8  g_pm1_evt_len, g_pm1_cnt_len;
static UINT32 g_smi_cmd; static UINT8 g_acpi_enable, g_acpi_disable;
static UINT64 g_reset_reg; static UINT8 g_reset_space, g_reset_width, g_reset_val;
static int    g_reset_ok;
static char   g_oem[8];
static int    g_present;

static UINT8  rd8(UINT64 p)  { return *(volatile UINT8 *)(UINTN)p; }
static UINT32 rd32(UINT64 p) { return *(volatile UINT32 *)(UINTN)p; }
static UINT64 rd64(UINT64 p) { return *(volatile UINT64 *)(UINTN)p; }

static int sig_eq(UINT64 p, const char *s) { return memcmp((void*)(UINTN)p, s, 4) == 0; }

static int rsdp_ok(UINT64 p) {
    if (!p) return 0;
    if (!sig_eq(p, "RSD PTR ")) return 0;
    UINT8 sum = 0;
    for (int i = 0; i < 20; i++) sum = (UINT8)(sum + rd8(p + i));
    if (sum != 0) return 0;
    UINT8 rev = rd8(p + 15);
    if (rev >= 1) {                       // ACPI 2.0+: uzun checksum
        UINT32 len = rd32(p + 20);
        if (len < 36 || len > 4096) return 0;
        UINT8 sum2 = 0;
        for (UINT32 i = 0; i < len; i++) sum2 = (UINT8)(sum2 + rd8(p + i));
        if (sum2 != 0) return 0;
    }
    return 1;
}

// Genel amacli adres yapisi (GAS): Address Space ID 1 = System I/O istiyoruz.
static UINT64 gas_io(UINT64 p, UINT32 bit_width) {
    UINT8 space = rd8(p), width = rd8(p + 1);
    UINT64 addr = rd64(p + 4);
    if (space != 1 || width == 0 || (bit_width && width != bit_width)) return 0;
    return addr;
}

static void parse_fadt(UINT64 f) {
    g_fadt = f;
    g_smi_cmd       = rd32(f + 48);
    g_acpi_enable   = rd8 (f + 52);
    g_acpi_disable  = rd8 (f + 53);
    UINT64 x;
    g_pm1a_evt = rd32(f + 56); g_pm1b_evt = rd32(f + 60);
    g_pm1a_cnt = rd32(f + 64); g_pm1b_cnt = rd32(f + 68);
    g_pm1_evt_len = rd8(f + 88); g_pm1_cnt_len = rd8(f + 90);
    if ((x = gas_io(f + 144, 32))) g_pm1a_evt = x;   // X_PM1a_EVT_BLK
    if ((x = gas_io(f + 156, 32))) g_pm1b_evt = x;
    if ((x = gas_io(f + 168, 16))) g_pm1a_cnt = x;   // X_PM1a_CNT_BLK
    if ((x = gas_io(f + 180, 16))) g_pm1b_cnt = x;
    g_facs = rd32(f + 36);  g_dsdt = rd32(f + 40);
    if (rd8(f + 8) >= 2) {  // ACPI 2.0+
        UINT64 xf = rd64(f + 128); if (xf) g_facs = xf;
        UINT64 xd = rd64(f + 136); if (xd) g_dsdt = xd;
    }
    // FADT flags (ofset 111) bit10 RESET_REG_SUP: ACPI 2.0+ reset mekanizmasi.
    g_reset_ok = 0;
    if ((rd32(f + 111) & (1u << 10)) && rd8(f + 8) >= 2) {
        UINT8 space = rd8(f + 112), width = rd8(f + 113);
        UINT64 addr = rd64(f + 116);
        if (addr && (space == 1 || space == 0) && (width == 8 || width == 16 || width == 32)) {
            g_reset_space = space; g_reset_width = width; g_reset_reg = addr;
            g_reset_val = rd8(f + 124); g_reset_ok = 1;
        }
    }
}


// ------------------------------------------------------------------ MADT (APIC tablosu)
// Sadece OKUR ve raporlar: CPU sayisi, Local APIC adresi, I/O APIC'ler, IRQ yonlendirme
// istisnalari (ISO). Kernel hala 8259 PIC ile calisir; APIC moduna gecis henuz yok.
#define MAX_IOAPIC 8
#define MAX_ISO    16
typedef struct { UINT8 id; UINT32 addr, gsi_base; } ioapic_t;
typedef struct { UINT8 src; UINT32 gsi; UINT16 flags; } iso_t;
static int g_ncpu, g_ncpu_disabled, g_nioapic, g_niso, g_pcat;
static UINT32 g_lapic32; static UINT64 g_lapic;
static ioapic_t g_ioapic[MAX_IOAPIC];
static iso_t    g_iso[MAX_ISO];

static int table_sum_ok(UINT64 p, UINT32 len) {
    UINT8 s = 0;
    for (UINT32 i = 0; i < len; i++) s = (UINT8)(s + rd8(p + i));
    return s == 0;
}

static int parse_madt(UINT64 m) {
    UINT32 len = rd32(m + 4);
    if (len < 44 || len > 65536 || !table_sum_ok(m, len)) return 0;
    g_lapic = g_lapic32 = rd32(m + 36);
    g_pcat = (rd32(m + 40) & 1u) ? 1 : 0;
    UINT32 off = 44;
    while (off + 2 <= len) {
        UINT8 type = rd8(m + off), elen = rd8(m + off + 1);
        if (elen < 2 || off + elen > len) break;             // bozuk girdi: dur
        UINT64 e = m + off;
        if (type == 0 && elen >= 8) {                         // Processor Local APIC
            UINT32 fl = rd32(e + 4);
            if (fl & 3u) g_ncpu++; else g_ncpu_disabled++;    // bit0 etkin, bit1 cevrimici olabilir
        } else if (type == 9 && elen >= 16) {                 // Processor Local x2APIC
            UINT32 fl = rd32(e + 8);
            if (fl & 3u) g_ncpu++; else g_ncpu_disabled++;
        } else if (type == 1 && elen >= 12 && g_nioapic < MAX_IOAPIC) {
            g_ioapic[g_nioapic].id = rd8(e + 2);
            g_ioapic[g_nioapic].addr = rd32(e + 4);
            g_ioapic[g_nioapic].gsi_base = rd32(e + 8);
            g_nioapic++;
        } else if (type == 2 && elen >= 10 && g_niso < MAX_ISO) {
            g_iso[g_niso].src = rd8(e + 3);
            g_iso[g_niso].gsi = rd32(e + 4);
            g_iso[g_niso].flags = (UINT16)(rd8(e + 8) | (rd8(e + 9) << 8));
            g_niso++;
        } else if (type == 5 && elen >= 12) {                 // 64 bit Local APIC adres gecersiz kilma
            g_lapic = rd64(e + 4);
        }
        off += elen;
    }
    return 1;
}

int    acpi_cpu_count(void)    { return g_ncpu; }
int    acpi_cpu_disabled(void) { return g_ncpu_disabled; }
UINT64 acpi_lapic_addr(void)   { return g_lapic; }
int    acpi_pcat_compat(void)  { return g_pcat; }
int    acpi_ioapic_count(void) { return g_nioapic; }
int    acpi_ioapic_get(int i, UINT8 *id, UINT32 *addr, UINT32 *gsi_base) {
    if (i < 0 || i >= g_nioapic) return 0;
    if (id)       *id       = g_ioapic[i].id;
    if (addr)     *addr     = g_ioapic[i].addr;
    if (gsi_base) *gsi_base = g_ioapic[i].gsi_base;
    return 1;
}
int    acpi_iso_count(void)    { return g_niso; }
int    acpi_iso_get(int i, UINT8 *src, UINT32 *gsi, UINT16 *flags) {
    if (i < 0 || i >= g_niso) return 0;
    if (src)   *src   = g_iso[i].src;
    if (gsi)   *gsi   = g_iso[i].gsi;
    if (flags) *flags = g_iso[i].flags;
    return 1;
}

void acpi_dump(void) {
    kprintf("ACPI: %s, OEM %s\n", g_present ? "var" : "yok", acpi_oem());
    if (!g_madt) { kprintf("MADT: yok\n"); return; }
    kprintf("MADT: %d CPU etkin (+%d devre disi), Local APIC 0x%llx, PC-AT uyumlu PIC: %s\n",
            g_ncpu, g_ncpu_disabled, g_lapic, g_pcat ? "evet" : "hayir");
    for (int i = 0; i < g_nioapic; i++)
        kprintf("  I/O APIC %d: adres 0x%x, GSI tabani %u\n", g_ioapic[i].id, g_ioapic[i].addr, g_ioapic[i].gsi_base);
    for (int i = 0; i < g_niso; i++)
        kprintf("  IRQ%u -> GSI %u (bayrak 0x%x)\n", g_iso[i].src, g_iso[i].gsi, g_iso[i].flags);
    kprintf("Not: kernel simdilik 8259 PIC ile calisir; APIC/SMP kullanilmiyor.\n");
}

static UINT64 g_sdt; static int g_sdt_x;

// Herhangi bir ACPI tablosunu imzasiyla bulur (RSDT/XSDT'yi yurur). Uzunluk ve saglama toplami dogrulanir.
// Donus: tablonun fiziksel adresi, yoksa/bozuksa 0. *len = tablo uzunlugu.
UINT64 acpi_find_table(const char sig[4], UINT32 *len) {
    if (!g_sdt) return 0;
    UINT32 tlen = rd32(g_sdt + 4);
    if (tlen < 36 || tlen > (1u << 20)) return 0;
    UINT64 ents = g_sdt_x ? (UINT64)((tlen - 36) / 8) : (UINT64)((tlen - 36) / 4);
    for (UINT64 i = 0; i < ents; i++) {
        UINT64 e = g_sdt_x ? rd64(g_sdt + 36 + i * 8) : rd32(g_sdt + 36 + i * 4);
        if (!e || !sig_eq(e, sig)) continue;
        UINT32 l = rd32(e + 4);
        if (l < 36 || l > (4u << 20) || !table_sum_ok(e, l)) continue;
        if (len) *len = l;
        return e;
    }
    return 0;
}

int acpi_init(UINT64 rsdp_phys) {
    g_rsdp = rsdp_phys;
    if (!rsdp_ok(g_rsdp)) { g_present = 0; return 0; }
    for (int i = 0; i < 6; i++) g_oem[i] = (char)rd8(g_rsdp + 9 + i);
    g_oem[6] = 0;

    UINT64 xsdt = 0, rsdt = 0;
    if (rd8(g_rsdp + 15) >= 1) xsdt = rd64(g_rsdp + 24);
    rsdt = rd32(g_rsdp + 16);

    UINT64 table = xsdt ? xsdt : rsdt;
    if (!table) { g_present = 0; return 0; }
    g_sdt = table; g_sdt_x = xsdt ? 1 : 0;
    UINT32 tlen = rd32(table + 4);
    if (tlen < 36 || tlen > (1u << 20)) { g_present = 0; return 0; }   // bozuk RSDT/XSDT: alt tasma/dongu korumasi
    UINT64 ents = xsdt ? (UINT64)((tlen - 36) / 8) : (UINT64)((tlen - 36) / 4);
    for (UINT64 i = 0; i < ents; i++) {
        UINT64 e = xsdt ? rd64(table + 36 + i * 8) : rd32(table + 36 + i * 4);
        if (!e) continue;
        if (!g_fadt && sig_eq(e, "FACP")) parse_fadt(e);
        if (!g_madt && sig_eq(e, "APIC") && parse_madt(e)) g_madt = e;
    }
    g_present = g_fadt != 0;
    return g_present;
}

int      acpi_present(void) { return g_present; }
UINT64   acpi_madt(void)    { return g_madt; }
const char *acpi_oem(void)  { return g_present ? g_oem : "-"; }

static void sci_enable(void) {
    if (!g_smi_cmd || !g_acpi_enable || !g_pm1a_cnt) return;
    if (inw((UINT16)g_pm1a_cnt) & 1) return;          // SCI_EN zaten acik
    outb((UINT16)g_smi_cmd, g_acpi_enable);
    for (int i = 0; i < 300; i++) {                   // ~3 sn bekle
        if (inw((UINT16)g_pm1a_cnt) & 1) return;
        thread_sleep_ms(10);
    }
}

void acpi_poweroff(void) {
    if (g_present && g_pm1a_cnt) {
        sci_enable();
        // _S5 objesini AML ile cozmedigimiz icin yaygin varsayim: SLP_TYP=5, SLP_EN=bit13,
        // SCI_EN ve kontrol bitleri korunur. Cogu makinede calisir; cozulmezse asagidaki portlara duser.
        UINT16 v5 = (UINT16)((5u << 10) | (1u << 13));
        outw((UINT16)g_pm1a_cnt, (UINT16)((inw((UINT16)g_pm1a_cnt) & 0xE003) | v5));
        if (g_pm1b_cnt) outw((UINT16)g_pm1b_cnt, (UINT16)((inw((UINT16)g_pm1b_cnt) & 0xE003) | v5));
        thread_sleep_ms(50);
    }
    // ACPI yok/ ise yaramadi: QEMU/Bochs/VirtualBox portlari (eski davranis).
    outw(0x604, 0x2000);
    outw(0xB004, 0x2000);
    outw(0x4004, 0x3400);
    __asm__ __volatile__ ("cli");
    for (;;) __asm__ __volatile__ ("hlt");
}

void acpi_reboot(void) {
    if (g_reset_ok) {
        if (g_reset_space == 1) {              // System I/O
            if (g_reset_width == 8)       outb((UINT16)g_reset_reg, g_reset_val);
            else if (g_reset_width == 16) outw((UINT16)g_reset_reg, g_reset_val);
            else                          outl((UINT16)g_reset_reg, g_reset_val);
        } else {                               // System Memory (birebir esleme)
            if (g_reset_width == 8)       *(volatile UINT8  *)(UINTN)g_reset_reg = g_reset_val;
            else if (g_reset_width == 16) *(volatile UINT16 *)(UINTN)g_reset_reg = g_reset_val;
            else                          *(volatile UINT32 *)(UINTN)g_reset_reg = g_reset_val;
        }
        for (volatile int i = 0; i < 1000000; i++) { }   // donanimin reset'i almasi icin kisa bekleme
    }
    outb(0x64, 0xFE);                              // 8042 yolu (varsa) - genel fallback
    for (;;) __asm__ __volatile__ ("hlt");
}

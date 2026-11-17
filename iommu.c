// DEOS -- IOMMU tespiti (bkz. iommu.h). Intel: DMAR tablosu + her DRHD biriminin VER/CAP/ECAP/GSTS yazmaclari.
// AMD: IVRS tablosundaki IVHD bloklari (yazmaclar okunmaz).
#include "iommu.h"
#include "acpi.h"
#include "paging.h"
#include "string.h"
#include "kprintf.h"

#define MAX_UNITS 8
static int g_kind, g_nunit, g_rmrr, g_haw, g_done;
static iommu_unit_t g_unit[MAX_UNITS];
static char g_name[96] = "Yok";

static UINT8  r8(UINT64 p)  { return *(volatile UINT8  *)(UINTN)p; }
static UINT16 r16(UINT64 p) { return (UINT16)(r8(p) | (r8(p + 1) << 8)); }
static UINT32 r32(UINT64 p) { return (UINT32)(r16(p) | ((UINT32)r16(p + 2) << 16)); }
static UINT64 r64(UINT64 p) { return (UINT64)r32(p) | ((UINT64)r32(p + 4) << 32); }

// VT-d yazmaclari: VER 0x00, CAP 0x08, ECAP 0x10, GSTS 0x1C (hepsi salt okuma).
static void read_intel_regs(iommu_unit_t *u) {
    volatile UINT8 *m = (volatile UINT8 *)mmio_map(u->base, 0x1000);
    if (!m) return;
    UINT32 ver = *(volatile UINT32 *)(m + 0x00);
    if (ver == 0xFFFFFFFFu || ver == 0) return;                    // aygit cevap vermiyor
    u->ver_major = (UINT8)((ver >> 4) & 0xF); u->ver_minor = (UINT8)(ver & 0xF);
    u->cap  = (UINT64)*(volatile UINT32 *)(m + 0x08) | ((UINT64)*(volatile UINT32 *)(m + 0x0C) << 32);
    u->ecap = (UINT64)*(volatile UINT32 *)(m + 0x10) | ((UINT64)*(volatile UINT32 *)(m + 0x14) << 32);
    UINT32 gsts = *(volatile UINT32 *)(m + 0x1C);
    u->translation_on = (gsts >> 31) & 1;
    u->intr_remap_on  = (gsts >> 25) & 1;
    u->regs_ok = 1;
}

static void parse_dmar(UINT64 t, UINT32 len) {
    g_kind = IOMMU_INTEL;
    g_haw = (int)r8(t + 36) + 1;
    UINT32 off = 48;
    while (off + 4 <= len) {
        UINT16 type = r16(t + off), elen = r16(t + off + 2);
        if (elen < 4 || off + elen > len) break;                    // bozuk girdi: dur
        if (type == 0 && elen >= 16 && g_nunit < MAX_UNITS) {       // DRHD
            iommu_unit_t *u = &g_unit[g_nunit++];
            memset(u, 0, sizeof *u);
            u->flags = r8(t + off + 4);
            u->segment = r16(t + off + 6);
            u->base = r64(t + off + 8);
            if (u->base) read_intel_regs(u);
        } else if (type == 1) {                                     // RMRR
            g_rmrr++;
        }
        off += elen;
    }
}

static void parse_ivrs(UINT64 t, UINT32 len) {
    g_kind = IOMMU_AMD;
    UINT32 off = 48;
    while (off + 4 <= len) {
        UINT8 type = r8(t + off); UINT16 elen = r16(t + off + 2);
        if (elen < 4 || off + elen > len) break;
        if ((type == 0x10 || type == 0x11 || type == 0x40) && elen >= 24 && g_nunit < MAX_UNITS) {   // IVHD
            iommu_unit_t *u = &g_unit[g_nunit++];
            memset(u, 0, sizeof *u);
            u->flags = r8(t + off + 1);
            u->base = r64(t + off + 8);
            u->segment = r16(t + off + 20);
        } else if (type == 0x20 || type == 0x21 || type == 0x22) { // IVMD (ayrilmis bellek)
            g_rmrr++;
        }
        off += elen;
    }
}

int iommu_detect(void) {
    if (g_done) return g_kind != IOMMU_NONE;
    g_done = 1; g_kind = IOMMU_NONE; g_nunit = g_rmrr = g_haw = 0;
    strncpy(g_name, "Yok (ACPI'de DMAR/IVRS tablosu yok)", sizeof g_name - 1);
    UINT32 len = 0;
    UINT64 t = acpi_find_table("DMAR", &len);
    if (t && len >= 48) {
        parse_dmar(t, len);
        int on = 0, ok = 0;
        for (int i = 0; i < g_nunit; i++) { if (g_unit[i].translation_on) on++; if (g_unit[i].regs_ok) ok++; }
        ksnprintf(g_name, sizeof g_name, "Intel VT-d: %d birim, %d bit adres, DMA remapping %s",
                  g_nunit, g_haw, !ok ? "okunamadi" : on ? "ACIK (firmware)" : "kapali");
        return 1;
    }
    t = acpi_find_table("IVRS", &len);
    if (t && len >= 48) {
        parse_ivrs(t, len);
        ksnprintf(g_name, sizeof g_name, "AMD-Vi (IVRS): %d IOMMU blogu", g_nunit);
        return 1;
    }
    return 0;
}

int         iommu_kind(void)       { iommu_detect(); return g_kind; }
const char *iommu_name(void)       { iommu_detect(); return g_name; }
int         iommu_unit_count(void) { iommu_detect(); return g_nunit; }
int         iommu_rmrr_count(void) { iommu_detect(); return g_rmrr; }
int         iommu_host_width(void) { iommu_detect(); return g_haw; }
int         iommu_unit_info(int i, iommu_unit_t *out) {
    iommu_detect();
    if (i < 0 || i >= g_nunit) return 0;
    if (out) *out = g_unit[i];
    return 1;
}

void iommu_dump(void) {
    iommu_detect();
    kprintf("IOMMU: %s\n", g_name);
    for (int i = 0; i < g_nunit; i++) {
        const iommu_unit_t *u = &g_unit[i];
        if (g_kind == IOMMU_INTEL) {
            kprintf("  birim %d: taban 0x%llx, segment %u%s", i, u->base, u->segment, (u->flags & 1) ? ", tum PCI" : "");
            if (u->regs_ok)
                kprintf(", VT-d %u.%u, CAP 0x%llx, ECAP 0x%llx, remapping %s, kesme remapping %s\n", u->ver_major, u->ver_minor,
                        u->cap, u->ecap, u->translation_on ? "ACIK" : "kapali", u->intr_remap_on ? "ACIK" : "kapali");
            else kprintf(", yazmaclar okunamadi\n");
        } else {
            kprintf("  IVHD %d: taban 0x%llx, segment %u\n", i, u->base, u->segment);
        }
    }
    if (g_rmrr) kprintf("  ayrilmis bellek bolgesi (RMRR/IVMD): %d\n", g_rmrr);
    if (g_kind) kprintf("Not: DMA remapping bu surumde ETKINLESTIRILMEZ; yalnizca tespit/raporlama yapilir.\n");
    if (g_kind == IOMMU_INTEL) for (int i = 0; i < g_nunit; i++) if (g_unit[i].translation_on) {
        kprintf("UYARI: firmware DMA remapping'i acik birakmis; aygit DMA'si (AHCI/NVMe/USB/ag) engellenebilir.\n"); break;
    }
}

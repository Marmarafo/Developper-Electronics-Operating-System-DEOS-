// DEOS -- IOMMU tespiti ve raporlama: Intel VT-d (ACPI DMAR) ve AMD-Vi (ACPI IVRS).
// SADECE okur: DMA remapping ETKINLESTIRILMEZ (sayfa tablolari/baglam girdileri kurulmaz). Bu surumde
// amac: gercek tabloyu ve donanim yazmaclarini dogru okumak, firmware'in DMA remapping'i acik birakip
// birakmadigini gostermek (acik birakmissa surucu DMA'si engellenebilir).
#ifndef DEOS_IOMMU_H
#define DEOS_IOMMU_H
#include "types.h"

enum { IOMMU_NONE = 0, IOMMU_INTEL = 1, IOMMU_AMD = 2 };

typedef struct {
    UINT64 base;                 // yazmac tabani (DRHD / IVHD)
    UINT16 segment;
    UINT8  flags;                // DRHD: bit0 = INCLUDE_PCI_ALL
    int    regs_ok;              // yazmaclar okunabildi (Intel)
    UINT8  ver_major, ver_minor; // VT-d surumu (VER yazmaci)
    UINT64 cap, ecap;            // CAP / ECAP yazmaclari
    int    translation_on;       // GSTS.TES: DMA remapping su an ACIK mi
    int    intr_remap_on;        // GSTS.IRES
} iommu_unit_t;

int         iommu_detect(void);                       // 1 = DMAR veya IVRS tablosu var
int         iommu_kind(void);                         // IOMMU_NONE / IOMMU_INTEL / IOMMU_AMD
const char *iommu_name(void);                         // kisa aciklama (hic NULL donmez)
int         iommu_unit_count(void);
int         iommu_unit_info(int i, iommu_unit_t *out);
int         iommu_rmrr_count(void);                   // DMAR: ayrilmis bellek bolgesi (RMRR) sayisi
int         iommu_host_width(void);                   // DMAR: fiziksel adres genisligi (bit); 0 = bilinmiyor
void        iommu_dump(void);                         // 'iommu' kabuk komutu

#endif

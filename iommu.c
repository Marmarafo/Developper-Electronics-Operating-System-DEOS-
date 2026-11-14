// DEOS -- IOMMU/VT-d tespiti (sadece tespit, henuz etkinlestirme yok)
#include "iommu.h"
#include "types.h"
#include "pci.h"
#include "kprintf.h"

static int g_iommu;
static const char *g_name;

int iommu_detect(void) {
    // Intel VT-d: ACPI DMAR tablosu aranir; simdilik PCI class 08:06:00 (Platform Security Device)
    // veya vendor-specific yollarla yaklasik tespit.
    // Gercek VT-d tespiti DMAR ACPI tablosu gerektirir (Adim 21'de tamamlanacak).
    for (int i = 0; i < pci_count(); i++) {
        const pci_dev_t *d = pci_get(i);
        if (d->vendor == 0x8086) {
            // Intel platform controller hub'larinda VT-d capability
            if (d->class_code == 0x08 && d->subclass == 0x06) {
                g_iommu = 1; g_name = "Intel VT-d (DMAR mevcut)";
                return 1;
            }
        }
    }
    g_iommu = 0; g_name = "Yok";
    return 0;
}

const char *iommu_name(void) { return g_name; }

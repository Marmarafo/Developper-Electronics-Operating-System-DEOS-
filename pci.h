#ifndef DEOS_PCI_H
#define DEOS_PCI_H
#include "types.h"

typedef struct {
    UINT8  bus, dev, fn;
    UINT16 vendor, device;
    UINT8  class_code, subclass, prog_if, header_type;
} pci_dev_t;

void   pci_scan(void);                                   // tum veriyolunu tarar (config space, 0xCF8/0xCFC)
int    pci_count(void);
const pci_dev_t *pci_get(int index);
const pci_dev_t *pci_find_class(UINT8 cls, UINT8 sub, int prog_if);   // prog_if < 0: herhangi
UINT32 pci_read32(const pci_dev_t *d, UINT8 off);
void   pci_write32(const pci_dev_t *d, UINT8 off, UINT32 v);
void   pci_enable(const pci_dev_t *d);                   // bellek erisimi + bus master (DMA)
UINT64 pci_bar(const pci_dev_t *d, int n);               // bellek BAR'inin taban adresi (0 = I/O veya yok)
const char *pci_class_name(UINT8 cls, UINT8 sub);

#endif

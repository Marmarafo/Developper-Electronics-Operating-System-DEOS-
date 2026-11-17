#ifndef DEOS_AHCI_H
#define DEOS_AHCI_H

// AHCI (SATA) denetleyici suruculeri: polling, tek komut yuvasi, DMA. Bulunan her SATA diski
// blk katmanina "sd0", "sd1".. olarak kaydeder ve bolumlerini tarar. Bulunan disk sayisini dondurur.
int ahci_init(void);

#endif

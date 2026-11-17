#ifndef DEOS_SELFTEST_H
#define DEOS_SELFTEST_H
// PMM + heap otomatik testi. Hepsi gecerse 1 doner.
int selftest_memory(void);
#include "bootinfo.h"
// Sayfalama testi (izinler, NULL korumasi, WC, map/unmap).
int selftest_paging(BootInfo *bi);
// Thread / zamanlayici testi (preemption, sleep, kill, kaynak iadesi).
int selftest_threads(void);
// Kullanici modu: ELF yukleme, izolasyon, hata yakalama, kaynak iadesi.
int selftest_user(void);
// Depolama: AHCI okuma, bolumler, FAT, dosya okuma. arg: bos | "write <aygit>" (son sektoru yazip geri yukler).
int selftest_storage(const char *arg);
// Dosya sistemi YAZMA testi (veri diskinde, yoksa kokte gecici dizinde; sonunda hepsini siler).
int selftest_fs(void);
// USB: cihazlar + (varsa) USB depolama okuma/yazma/dosya sistemi.
int selftest_usb(void);
// Gercek donanima hazirlik testleri: ACPI/MADT, CPU koruma, klavye duzenleri, ELF (bozuk girdiler), ag, NVMe, IOMMU.
// arg "write": NVMe'ye yazan testler de calisir (diskin son sektorlerini yazip geri yukler -- yalniz test diskinde!).
int selftest_hw(const char *arg);
// Hepsini sirayla calistirir (kabukta 'selftest'; BOOT_SELFTEST=1 ile acilista). 1 = hepsi gecti.
int selftest_all(BootInfo *bi, int allow_write);
#endif

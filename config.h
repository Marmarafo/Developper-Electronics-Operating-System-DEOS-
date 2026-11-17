// DEOS -- yapilandirma.
#ifndef DEOS_CONFIG_H
#define DEOS_CONFIG_H

// Surum adlandirmasi: Pr-N (N, her pakette artan tam sayi). Eski numarali surum sistemi kaldirildi.
#define DEOS_VERSION   "Pr-4"
#define DEOS_VERSION_L L"Pr-4"                // bootloader (UEFI, genis karakter) icin
#define DEOS_COPYRIGHT "Telif hakki (c) 2026 DEOS"

#define START_DESKTOP 1                      // 1: acilista masaustu, 0: sadece metin konsolu
#define TIMER_HZ      100                    // PIT frekansi (tick/sn); zaman asimlari buna gore hesaplanir

#ifndef BOOT_SELFTEST
#define BOOT_SELFTEST 0                      // 1: acilista tum testleri calistir, sonucu seri porta yaz, kapan (CI)
#endif

#endif

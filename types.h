// DEOS -- kernel'in tum dosyalarinin paylastigi temel tip tanimlari.
// Ayni tipler bootloader tarafinda efi.h icinde de (bagimsiz olarak)
// tanimli -- ayni DEOS_BASE_TYPES_DEFINED korumasini kullandiklarindan
// iki taraf da guvenle bir arada var olabilir (farkli derleme birimleri).
#ifndef DEOS_TYPES_H
#define DEOS_TYPES_H

#ifndef DEOS_BASE_TYPES_DEFINED
#define DEOS_BASE_TYPES_DEFINED
typedef unsigned char      UINT8;
typedef unsigned short     UINT16;
typedef unsigned int       UINT32;
typedef unsigned long long UINT64;
typedef unsigned long long UINTN;
#endif

#endif

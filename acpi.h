// DEOS -- ACPI tablo ayrilstirma (RSDP -> XSDT/RSDT -> FADT/MADT) + S5 kapatma.
#ifndef DEOS_ACPI_H
#define DEOS_ACPI_H
#include "types.h"

int      acpi_init(UINT64 rsdp_phys);      // 1 = FADT bulundu (sayfalama aciktan SONRA cagrin)
int      acpi_present(void);
void     acpi_poweroff(void);              // donmez; ACPI yoksa bilinen emulasyon portlarina duser
UINT64   acpi_madt(void);                  // MADT fiziksel adresi (yoksa 0)
const char *acpi_oem(void);

#endif

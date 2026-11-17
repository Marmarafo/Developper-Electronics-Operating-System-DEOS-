// DEOS -- ACPI tablo ayrilstirma (RSDP -> XSDT/RSDT -> FADT/MADT) + S5 kapatma.
#ifndef DEOS_ACPI_H
#define DEOS_ACPI_H
#include "types.h"

int      acpi_init(UINT64 rsdp_phys);      // 1 = FADT bulundu (sayfalama aciktan SONRA cagrin)
int      acpi_present(void);
void     acpi_poweroff(void);              // donmez; ACPI yoksa bilinen emulasyon portlarina duser
void     acpi_reboot(void);                  // FADT RESET_REG (varsa), yoksa 8042 reset
UINT64   acpi_madt(void);                  // MADT fiziksel adresi (yoksa 0)
const char *acpi_oem(void);

// MADT'den (sadece okuma): CPU/APIC topolojisi. MADT yoksa/bozuksa hepsi 0.
int     acpi_cpu_count(void);                 // etkin islemci (Local APIC) sayisi
int     acpi_cpu_disabled(void);              // devre disi / eklenebilir islemci sayisi
UINT64  acpi_lapic_addr(void);                // Local APIC taban adresi
int     acpi_pcat_compat(void);               // 1 = 8259 PIC de var
int     acpi_ioapic_count(void);
int     acpi_ioapic_get(int i, UINT8 *id, UINT32 *addr, UINT32 *gsi_base);
int     acpi_iso_count(void);                 // ISA IRQ -> GSI yonlendirme istisnalari
int     acpi_iso_get(int i, UINT8 *src_irq, UINT32 *gsi, UINT16 *flags);
UINT64  acpi_find_table(const char sig[4], UINT32 *len);   // imza ("DMAR", "IVRS", ...) -> fiziksel adres, yoksa 0
void    acpi_dump(void);                      // 'acpi' kabuk komutu

#endif

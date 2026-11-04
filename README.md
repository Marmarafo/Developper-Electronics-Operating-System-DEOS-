# DEOS

x86_64 UEFI uzerinde calisan, sifirdan yazilmis bir isletim sistemi. Bootloader, kernel, suruculer,
dosya sistemi, pencere yoneticisi ve kullanici programlari bu depodadir.

## Degisiklik gunlugu
### v0.8 (gercek donanima hazirlik, 1. paket)
* Bootloader: GOP modu artik yalnizca 32bpp BGRX kabul eder; degilse uygun mod aranip `SetMode` ile degistirilir.
* Bootloader: bellek haritasi icin dinamik tampon (EFI_BUFFER_TOO_SMALL buyumesi), ExitBootServices 5 deneme.
* Bootloader: ACPI RSDP UEFI ConfigurationTable'dan alinip BootInfo.rsdp'ye tasinir.
* ACPI: RSDP/XSDT/RSDT dogrulama, FADT + MADT bulma, gercek S5 kapatma (SCI enable + PM1a/PM1b); ACPI yoksa eski emulasyon portlarina duser.
* Kesmeler: spurious IRQ 7/15 ayirmasi (IRQ7'ye EOI yok, IRQ15'te yalniz master EOI).
* NVMe: PCI 01:08:02 taranir; admin + I/O kuyruk, namespace 1, PRP listesi, 512B sektor, yazma sonrasi flush; `nvme0` blok aygiti + bolum tarama.
* io.h: `inw`; string: `strncpy` eklendi.

## Derleme ve calistirma
Gereksinimler (Ubuntu): `gcc` (x86_64-linux-gnu), `binutils`, `qemu-system-x86`, `ovmf`, `mtools`.

```
./build.sh          # bootloader (BOOTX64.EFI), kullanici programlari (ELF) ve kernel (KERNEL.BIN)
./test_in_qemu.sh   # disk imajlarini hazirlar (make_image.sh) ve QEMU'da calistirir
./test_debug.sh     # ayni, ek olarak kernel ciktisi terminale yazilir + triple fault gunlugu
```
Fare pencereye girince yakalanir; birakmak icin Ctrl+Alt+G.

## Diskler
* `esp.img` (her calistirmada yeniden olusur): onyukleme diski, FAT32. `\EFI\BOOT\BOOTX64.EFI`,
  `\EFI\BOOT\KERNEL.BIN`, `/BIN/*.elf` (kullanici programlari), `rootfs/` icindeki ornek dosyalar. Kernel'de `sd0`.
* `data.img` (bir kez olusur, KORUNUR): kalici veri diski, FAT32. Kernel'de `sd1`, `/mnt/sd1` altinda.
* Gercek bir diskte GPT/MBR bolumleri otomatik taranir (`sd0p1`, ...); FAT16/FAT32 hacimleri baglanir.

## Kabuk komutlari
Dosya sistemi: `ls [yol]`, `cd`, `pwd`, `cat <dosya>`, `touch`, `mkdir`, `rm`, `rmdir`, `write <dosya> <metin>`,
`append <dosya> <metin>`, `cp <kaynak> <hedef>`, `mv <kaynak> <hedef>`, `df`, `disks`, `mounts`.
Bosluklu adlar icin tirnak kullanin: `mkdir "Yeni Klasor"`.
Surecler ve sistem: `run <program|yol>`, `bg <program>`, `progs`, `ps`, `spawn`, `kill <id>`, `mem`, `pmm`, `heap`,
`vm`, `info`, `uptime`, `date`, `lspci`, `lsusb`, `mouse`, `echo`, `clear`, `reboot`.
Hata ayiklama: `crash [div|null|write|rodata|nx|stack|tstack]`.
Otomatik testler: `memtest`, `vmtest`, `threadtest`, `usertest`, `disktest [write <aygit>]`, `fstest`, `systest` (ring 3), `usbtest`.

## Kullanici programlari (ring 3)
`run sh` ile ring 3 kabugu: `cd`, `pwd`, `exit`, `>` / `>>` yonlendirme, `&` arka plan. Araclar `/bin` altinda:
`ls cat echo mkdir rm touch cp mv wc hexdump systest`. Syscall'lar: open/close/read/write/lseek/stat/readdir/mkdir/
unlink/rename/getcwd/chdir/spawn/wait (bkz. `syscall_abi.h`). Surec basina fd tablosu (16) ve calisma dizini vardir.

## USB
xHCI (USB 3.x/2.x) denetleyicisi, HID klavye/fare (boot protokolu, ABD duzeni, tus tekrari) ve USB depolama
(Bulk-Only/SCSI, FAT hacmi `/mnt/usbN` altina baglanir). `lsusb`, `usbtest`.
Bilinen sinirlar: hub'lar, EHCI/OHCI/UHCI, gercek donanimda test, calisirken takilan yeni cihazin taninmasi
(cikarma calisir; QEMU'da sonradan `device_add` ile takilan cihaz tanimadi, nedeni arastirilmadi).

## Masaustu uygulamalari
Terminal (kabuk), Dosyalar (gezinme, yeni klasor/dosya, sil, yeniden adlandir, program calistirma), Not Defteri
(Ctrl+S kaydet, Ctrl+O ac, Ctrl+N yeni, ok tuslari), Sistem Izleyici, Hesap Makinesi, Hakkinda.

## Dosya sistemi dogrulamasi
FAT yazma kodu, Linux'un kendi araclariyla capraz dogrulanir: DEOS ile dosya yazdiktan sonra
`fsck.fat -n data.img` temiz cikmali, `mdir -/ -i data.img ::` ve `mtype` ayni agaci ve icerigi gostermelidir.

## Kaynak duzeni
| Alan | Dosyalar |
|---|---|
| Onyukleme | `adim3.c`, `efi.h`, `bootinfo.h` |
| Temel | `entry.S`, `kernel.c`, `kernel.ld`, `types.h`, `cpu.h`, `io.h`, `string.c`, `kprintf.c`, `serial.c` |
| Kesmeler | `gdt.c` (GDT+TSS+IST), `idt.c`, `isr_stubs.S`, `irq*.{c,S}`, `pic.c`, `timer.c` (PIT) |
| Bellek | `pmm.c` (bitmap sayfa ayirici), `heap.c` (kmalloc), `paging.c` (4 seviyeli, NX/WP/PAT, MMIO) |
| Surecler | `thread.c` + `switch.S` (preemptive zamanlayici), `process.c`, `elf.c`, `syscall.c` + `usermode.S` |
| Depolama | `pci.c`, `ahci.c` (SATA, DMA), `blk.c` (blok katmani, GPT/MBR), `fat.c` (FAT16/32), `fs.c` (baglama noktalari) |
| Girdi | `keyboard.c` (PS/2), `mouse.c` (PS/2), `rtc.c`, `xhci.c` + `usb_hid.c` + `usb_msc.c` (USB) |
| Grafik / arayuz | `console.c`, `gfx.c` (cift tampon), `wm.c` (pencere yoneticisi), `apps.c` (uygulamalar) |
| Kullanici | `user/` (programlar + `ulib`), `syscall_abi.h`, `file.c` (acik dosya nesneleri) |
| Testler | `selftest.c` |

## Tasarim notlari
* Kernel `0x400000`'a yuklenir; bolumler 4 KB hizali: `.text` R-X, `.rodata` R--, `.data/.bss` RW-.
* Her surecin kendi PML4'u vardir; PML4[0] (kernel, birebir esleme) ortaktir ve supervisor-only'dir.
  Kullanici adres uzayi `0x8000000000` ve uzeridir.
* Syscall: `rax` numara; `rdi, rsi, rdx, r10, r8` arguman. Numaralar `syscall_abi.h` icinde.
* AHCI suruculeri polling ile calisir; DMA 64 KB'lik bounce tamponu uzerinden yapilir.
* FAT: okuma + yazma (dosya/dizin olusturma, silme, yeniden adlandirma, uzun adlar). Her yazma aninda diske gider
  (AHCI FLUSH); FSInfo guncellenir. Bir global kilit GUI/kabuk/sureclerin es zamanli erisimini korur.

## Yol haritasi
1. USB: hub destegi, sicak takma, EHCI; NVMe; ag (virtio-net / e1000); gercek donanim testleri
2. Kullanici kabugu icin pipe, ortam degiskenleri; malloc/stdio
4. Cok cekirdek (SMP, APIC), bellek koruma iyilestirmeleri, kullanici modu kabuk

# DEOS (Pr-4) -- gercek donanimda deneme rehberi

> **Durum:** DEOS suruculerinin hicbiri gercek makinede dogrulanmadi; tum testler QEMU/OVMF uzerindedir.
> Asagidaki adimlar *deneme* icindir. Ilk denemeyi **onemli verisi olmayan** bir makinede ve **harici bir USB bellekten**
> yapin. DEOS'u makinenin dahili diskine yazmayin.

## 1. Once QEMU'da dogrulayin
```
./ci_test.sh              # dort asama (qemu64, SMEP+SMAP'li CPU, SMAP ihlali, yigin tasmasi); cikis kodu 0 olmali
```

## 2. USB bellegi hazirlama (Linux)
`/dev/sdX` yerine **dogru** aygiti yazin (`lsblk` ile kontrol edin). Yanlis aygit = veri kaybi.
```
./build.sh
sudo sgdisk --zap-all /dev/sdX
sudo sgdisk -n1:0:0 -t1:EF00 /dev/sdX          # tek ESP bolumu
sudo mkfs.fat -F32 /dev/sdX1
sudo mount /dev/sdX1 /mnt
sudo mkdir -p /mnt/EFI/BOOT /mnt/BIN
sudo cp BOOTX64.EFI KERNEL.BIN /mnt/EFI/BOOT/
sudo cp build_user/*.elf /mnt/BIN/
sudo cp -r rootfs/* /mnt/
sudo umount /mnt
```
Bootloader `\EFI\BOOT\KERNEL.BIN` dosyasini arar.

## 3. Makine ayarlari
* **Secure Boot: kapali** (bootloader imzasizdir).
* CSM/Legacy boot kapali, saf UEFI. USB'den acilis menusu (F12/F8/Esc) ile secin.
* SATA modu **AHCI** (RAID/Intel RST degil). NVMe icin ek ayar gerekmez.
* xHCI "handoff" aciksa USB klavye/fare icin iyi olur.

## 4. Neler bekleyebilirsiniz / bilinen sorunlar
* **USB hub:** USB 2.0 hub'lar (ve hub arkasinda hub, 5 seviyeye kadar) desteklenir; hub basina ilk 8 port taranir. **USB 3.x (SuperSpeed) hub'lar
  desteklenmez**; bir dock'un USB 3.x hub'i arkasindaki cihazlar gorunmeyebilir. Emin olamazsaniz klavyeyi dogrudan makine portuna takin.
  `lsusb` komutu hub topolojisini `4.1`, `4.2` gibi yollarla gosterir.
* **PS/2 yok:** USB klavye ve fare kullanilir; fare olmadan da masaustu acilir.
* **APIC yok:** kernel 8259 PIC + PIT kullanir. 8259'u hic emule etmeyen cok yeni platformlarda kesme gelmeyebilir.
* **Ag:** yalniz Intel e1000/8254x(e) ailesi. Wi-Fi ve Realtek/diger NIC'ler yok. Acilista arka planda **DHCP** denenir (en fazla ~12 sn);
  basarisizsa statik 10.0.2.15 kalir: `netcfg A.B.C.D [maske] [gecit]` ile elle verin ya da `dhcp` komutunu yeniden calistirin. `ifconfig`/`netinfo` durumu gosterir.
* **Grafik:** UEFI GOP framebuffer'i (32bpp BGRX) kullanilir; GPU surucusu yoktur.
* **DMA:** AHCI/NVMe/e1000 tamponlari ilk 4 GB'ta ayrilir. Bellek ayirimi basarisiz olursa surucu o aygiti atlar.
* **NVMe:** namespace 1, 512 B sektor. 4 KB sektorlu (LBAF) disklerde surucu kendini devre disi birakir ve nedenini yazar.
* **IOMMU:** `iommu` komutu DMAR/IVRS tablosunu ve VT-d birim yazmaclarini gosterir (salt okuma). Firmware DMA remapping'i **acik** birakmissa
  ("remapping ACIK" uyarisi) aygit DMA'si (AHCI/NVMe/USB/ag) engellenebilir; bu durumu not edin. DEOS remapping'i etkinlestirmez ve kapatmaz.
* **Seri cikti:** kernel mesajlari COM1'e (0x3F8) de yazilir. Cogu modern dizustunde fiziksel seri port yoktur; ekrani fotografla kaydedin.
* **Korumalar:** SMEP/SMAP (islemci destekliyorsa) ve yigin canary'si aciktir. Bir panik ekrani "SMAP IHLALI" ya da "YIGIN TASMASI" derse lutfen aynen bildirin.

## 5. Guvenli test sirasi
1. Acilisi izleyin; hata iletisi varsa fotografini cekin.
2. `info`, `lspci`, `lsusb`, `acpi`, `iommu`, `disks`, `netinfo` komutlarinin ciktilarini kaydedin.
3. `hwtest` calistirin. Bu komut **diske yazmaz**: NVMe yazma testleri ve ring 3 testleri (kok dosya sistemine gecici dosya yazar) atlanir.
   Ag testleri (DHCP, ping; yanki sunucusu yoksa UDP/TCP/DNS atlanir), CPU/ACPI/klavye/ELF/IOMMU/USB hub testleri calisir. Tum `[FAIL]` satirlarini kaydedin.
4. Ag icin: `ping <gecit>`, `nslookup <ad>`; ring 3'te `run sh` ardindan `ifconfig`, `nslookup <ad>`, `nc <ana_makine> <port>`.
5. Yalnizca **kullanilabilir bir test diskinde**: `hwtest write` / `selftest write` / `disktest write <aygit>`. Bunlar diskin *son sektorlerini*
   yazip geri yukler (gercek disklerde orasi GPT yedek tablosudur) ve boot hacminin kokune gecici dosyalar (`/LIBTEST.TMP`, `/SHT.TMP`) yazip siler.
   Elektrik/reset kesilirse tablo ya da dosya sistemi bozulabilir.
6. `reboot` ve kapatma (ACPI S5) calisiyor mu deneyin.

## 6. Sonuc tablosu (kendi makinenizi ekleyin)
| Makine | Acilis | Klavye/fare | Hub | SATA | NVMe | USB depolama | Ag (DHCP/ping) | Kapatma/reset | Notlar |
|---|---|---|---|---|---|---|---|---|---|
| (ornek) | | | | | | | | | |

Bir sorun bulursaniz `lspci`, `lsusb`, `acpi`, `iommu`, `netinfo` ciktilarini ve ekran goruntusunu ekleyin; `ci_test.sh` ile QEMU'da yeniden
uretip bir test eklemek en kalici duzeltme yoludur.

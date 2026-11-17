# DEOS

x86_64 UEFI uzerinde calisan, sifirdan yazilmis bir isletim sistemi. Bootloader, kernel, suruculer, ag yigini,
dosya sistemi, pencere yoneticisi, kullanici alani (C kutuphanesi + kabuk) ve programlar bu depodadir.

**Surum: Pr-4** (`config.h`). Surumler `Pr-N` seklinde adlandirilir (N, her pakette artan tam sayi); eski numarali
surum sistemi kaldirilmistir.

> Gercek donanimda denemeden once `GERCEK_DONANIM.md` dosyasini okuyun. Hicbir surucu gercek makinede dogrulanmadi.

## Durum ozeti
| Alan | Durum | Not |
|---|---|---|
| UEFI onyukleme (GOP 32bpp, ACPI RSDP, bellek haritasi) | Calisiyor | QEMU/OVMF'de test edilir |
| Bellek (PMM, heap, 4 seviyeli sayfalama, NX/WP/PAT) | Calisiyor | |
| Surecler (preemptive thread, ring 3, ELF, syscall) | Calisiyor | |
| CPU korumalari: SMEP, **SMAP**, **yigin canary'si** (kernel + ring 3) | Calisiyor | CPU destekliyorsa SMEP/SMAP; canary her zaman. Kasitli hata testleriyle dogrulanir |
| AHCI (SATA) + NVMe + blok katmani (GPT/MBR) + FAT16/32 | Calisiyor | okuma + yazma |
| USB: xHCI, HID klavye/fare, depolama, **USB 2.0 hub** | Calisiyor (QEMU) | hub: en fazla 5 seviye, hub basina ilk 8 port; USB 3.x hub yok |
| Ethernet: Intel e1000, ARP, IPv4, ICMP | Calisiyor (QEMU) | giris basliklari tam dogrulanir |
| **UDP soketleri, DHCP (kira yenileme dahil), DNS (A), TCP istemcisi** | Calisiyor (QEMU) | TCP: yalniz connect; listen/accept yok |
| **Ring 3: C kutuphanesi (malloc, stdio, printf), pipe, ortam degiskenleri, soket API** | Calisiyor | kayan nokta yok |
| **Ring 3 kabugu: boru hatti, yonlendirme, $DEGISKEN, PATH** | Calisiyor | |
| ACPI: kapatma, yeniden baslatma, MADT/DMAR/IVRS okuma | Calisiyor | MADT yalnizca raporlanir |
| **IOMMU: Intel VT-d (DMAR) ve AMD-Vi (IVRS) gercek tablodan tespit** | Calisiyor (rapor) | DMA remapping ETKINLESTIRILMEZ |
| APIC / IOAPIC / SMP | **Yok** | kernel 8259 PIC + PIT ile tek cekirdek calisir |
| Turkce Q klavye (PS/2 + USB), TR/US gecisi (Alt+Shift) | Calisiyor | noktalama TR Q'ya yakindir, AltGr yok |
| Masaustu (pencere yoneticisi, 6 uygulama) | Calisiyor | |

## Derleme, calistirma, test
Gereksinimler (Ubuntu): `gcc-x86-64-linux-gnu`, `binutils-x86-64-linux-gnu`, `qemu-system-x86`, `ovmf`, `mtools`, `dosfstools`, `python3`.

```
./build.sh          # bootloader (BOOTX64.EFI), kullanici programlari (ELF) ve kernel (KERNEL.BIN)
./test_in_qemu.sh   # disk imajlarini hazirlar ve QEMU'da calistirir (AHCI + NVMe + USB + USB hub + e1000 + intel-iommu)
./test_debug.sh     # ayni, ek olarak kernel ciktisi terminale yazilir + triple fault gunlugu
./ci_test.sh        # otomatik test (asagida); cikis kodu 0 = hepsi gecti
./clean.sh          # uretilmis dosyalari siler
```
Fare pencereye girince yakalanir; birakmak icin Ctrl+Alt+G. Zip paketi hazir derlenmis `BOOTX64.EFI`, `KERNEL.BIN`,
`build_user/*.elf` ve `progs_table.c` dosyalarini da icerir; `./build.sh` bunlari yeniden uretir.

Derleme secenekleri (ortam degiskeni): `BOOT_SELFTEST=1` acilista tum testleri calistirip sonucu seri porta yazan ve
makineyi kapatan kernel; `BOOT_CRASH=smap|canary|...` acilista kasitli hata; `EXTRA_CFLAGS="-D..."`.

### ci_test.sh
Dort asamada derler ve QEMU'yu basliksiz (headless) calistirir:

| Asama | Ne yapar |
|---|---|
| `selftest-q64` | `BOOT_SELFTEST=1`, CPU `qemu64` (SMEP/SMAP yok): 435 kontrol |
| `selftest-max` | ayni testler, CPU `max` (SMEP + SMAP acik) |
| `crash-smap` | kernel, kullanici sayfasini `stac`'siz okur: **"SMAP IHLALI"** panigi beklenir |
| `crash-canary` | kernel yerel tamponu tasirir: **"YIGIN TASMASI"** panigi beklenir |

Host'ta `ci/echo_server.py` (UDP+TCP yanki + basit DNS yanitlayicisi) baslatilir; guest, QEMU slirp uzerinden
`10.0.2.2:47007`'ye baglanarak gercek UDP/TCP/DNS trafigi dogrular. Ortam degiskenleri: `STAGES="selftest-q64 ..."` (alt kume),
`TIMEOUT=sn`, `ACCEL=kvm`, `ECHO_PORT`. Cikis kodlari: 0 gecti, 1 basarisiz asama var, 3 ortam eksik.
CI modunda beklenen aygitlar (ACPI/MADT, NVMe, e1000, intel-iommu, USB hub, yanki sunucusu) yoksa test **basarisiz** sayilir;
normal kullanimda ilgili bolum "atlandi" notuyla gecilir.

## Diskler
* `esp.img` (her calistirmada yeniden olusur): onyukleme diski, FAT32. `\EFI\BOOT\BOOTX64.EFI`, `\EFI\BOOT\KERNEL.BIN`,
  `/BIN/*.elf` (kullanici programlari), `rootfs/` icindeki ornek dosyalar. Kernel'de `sd0`.
* `data.img` (bir kez olusur, KORUNUR): kalici veri diski, FAT32. `sd1`, `/mnt/sd1` altinda.
* `usb.img`, `usb2.img` (bir kez olusur): USB deneme diskleri (`usb2.img` hub arkasinda). `usb0`, `usb1`; `/mnt/usbN` altinda.
* `nvme.img` (bir kez olusur): NVMe deneme diski (`-device nvme`). `nvme0`.
* Gercek bir diskte GPT/MBR bolumleri otomatik taranir (`sd0p1`, ...); FAT16/FAT32 hacimleri baglanir.

## Cekirdek kabugu (metin konsolu / masaustu terminali)
Dosya sistemi: `ls [yol]`, `cd`, `pwd`, `cat`, `touch`, `mkdir`, `rm`, `rmdir`, `write <dosya> <metin>`, `append`, `cp`, `mv`,
`df`, `disks`, `mounts`. Bosluklu adlar icin tirnak kullanin.

Surecler ve sistem: `run <program|yol>`, `bg <program>`, `progs`, `ps`, `spawn`, `kill <id>`, `mem`, `pmm`, `heap`, `vm`, `info`,
`uptime`, `date`, `lspci`, `lsusb` (hub yolu `4.1` seklinde), `acpi`, `iommu`, `mouse`, `echo`, `clear`, `reboot`.

Ag: `ping A.B.C.D`, `netinfo`, `netcfg A.B.C.D [maske veya /N] [gecit]`, `dhcp`, `nslookup <ad>`, `netstat`.

Hata ayiklama: `crash [div|null|write|rodata|nx|stack|tstack|smap|canary]`.

Testler: `memtest`, `vmtest`, `threadtest`, `usertest`, `disktest [write <aygit>]`, `fstest`, `systest`, `usbtest`,
`hwtest [write]`, `selftest [write]`. **`write` argumanli komutlar diske yazar** (NVMe testi diskin son sektorlerini yazip geri
yukler; ring 3 testleri kok dosya sistemine gecici dosya yazar): yalniz test disklerinde kullanin.

## Ring 3: kabuk, programlar, kutuphane
`run sh` ile ring 3 kabugu (`sh -c "satir"` tek satir calistirip cikar):
* **Boru hatti:** `ls | grep elf | wc` (en fazla 8 asama); **yonlendirme:** `< dosya`, `> dosya`, `>> dosya`; **arka plan:** `&`.
* **Degiskenler:** `$AD`, `${AD}`, `$?`; ic komutlar `export AD=DEGER`, `unset AD`, `env`, `cd`, `pwd`, `exit`, `help`.
* Programlar **PATH** (varsayilan `/bin`, `:` ile ayrilmis dizinler) icindeki `<ad>.elf` dosyalarindan, sonra gomulu programlardan bulunur.
  Ortam degiskenleri `spawn` ile cocuga kopyalanir (cekirdekte surec basina 512 bayt).

Programlar (`/bin`): `ls cat echo mkdir rm touch cp mv wc hexdump grep head env ifconfig nslookup nc systest libtest nettest smash`.
Gomulu: `hello ticker heaptest reverse badboy fault`. `cat` ve `wc` dosya verilmezse stdin'i okur (boru hatti).
`nc [-u] [-w sn] [-s metin] ana_makine port` basit netcat istemcisidir (stdin'in tamamini ya da `-s` metnini gonderir, yaniti yazar).

**C kutuphanesi** (`user/ulib.*`, `ustdlib.c`, `ustdio.c`; kayan nokta, locale, sinyal, setjmp yok):
`malloc/free/calloc/realloc` (sbrk uzerinde, 16 bayt hizali, bitisik bos bloklari birlestirir, cift `free`'yi yakalar),
`printf/fprintf/snprintf/vsnprintf` (`%d %i %u %x %X %o %c %s %p %%`, `l ll z h hh`, genislik, `*`, hassasiyet, `- 0 + # ` bayraklari),
bufferli `FILE` (`fopen/fclose/fread/fwrite/fgets/fputs/fgetc/ungetc/fseek/ftell/rewind/fflush/feof/ferror`, kipler `r w a r+ w+ a+`),
`getenv/setenv/unsetenv`, `strtol/strtoul/atoi`, `qsort/bsearch`, `rand/srand`, ctype, dizge islevleri.
Eski `u_printf`/`sys_*` arayuzu de calismaya devam eder.

**Syscall'lar** (`syscall_abi.h`): exit, write, getpid, sleep, yield, ticks, getchar, sbrk, open, close, read, lseek, stat, readdir, mkdir,
unlink, rename, getcwd, chdir, spawn(fdmap), wait, **getrandom, socket, bind, connect, sendto, recvfrom, sockopt, resolve, netinfo,
pipe, getenv, setenv, environ**. Surec basina fd tablosu (16); fd'ler dosya, dizin, konsol, **pipe ucu ya da soket** olabilir.

**Soketler:** `socket(SOCK_STREAM)` TCP istemcisi (`connect` bloklar, en fazla 10 sn), `socket(SOCK_DGRAM)` UDP (`bind`, `connect`,
`sendto`, `recvfrom`; datagram en fazla 1472 bayt). `SO_RCVTIMEO` (varsayilan 5 sn). Soketler `read`/`write` ile de kullanilir.
Surec acik soketle cikarsa cekirdek baglantiyi kapatir.

**Pipe:** 4 KB halka tampon; okuma veri ya da tum yazma uclari kapanana kadar bloklar (EOF = 0); okuyucu kalmayinca yazma `EPIPE` doner.
Uclar surecler arasi paylasilir: kabuk cocuga verdigi uclarin kendi kopyalarini kapatmalidir.

## USB
xHCI (USB 3.x/2.x) denetleyicisi, HID klavye/fare (boot protokolu, TR/US duzeni, tus tekrari), USB depolama (Bulk-Only/SCSI;
FAT hacmi `/mnt/usbN` altina baglanir) ve **USB 2.0 hub**: hub tanimlayicisi okunur, slot baglamina Hub/MTT/port sayisi/TTT yazilir,
portlar guclendirilir ve ~200 ms'de bir kontrol transferleriyle yoklanir (baglanti/cikarma, port sifirlama, hiz tespiti); hub arkasindaki
dusuk/tam hizli cihazlara TT bilgisi verilir; route string ile hub arkasinda hub (en fazla 5 seviye) calisir; hub cikarilinca alt cihazlar
da temizlenir. Sinirlar: USB 3.x (SuperSpeed) hub, hub basina 8'den fazla port, hub kesme uc noktasi (yoklama kullanilir),
EHCI/OHCI/UHCI yok; QEMU'da calisirken takilan yeni cihaz tanimiyor (cikarma calisir; nedeni arastirilmadi).

## Ag
Intel e1000/8254x (+e1000e): PCI 02:00, EEPROM'dan MAC, 16 tanimlayicili asenkron TX (her tanimlayicinin kendi tamponu), 128'lik RX.
Yigin yoklama tabanlidir (kesme/MSI yok): `net` thread'i RX'i, TCP zamanlayicilarini ve DHCP kirasini ~5 ms'de bir isler.

* **IPv4 girisi:** surum, IHL, toplam uzunluk (cerceve sinirlari), baslik saglamasi, parcalanmis paketler (reassembly yok) atilir; hedef IP
  filtrelenir (kendi IP'miz, yayin, ya da IP yokken). Gonderen MAC'i taze tutulur. Adresler `UINT32`, **ilk oktet dusuk bayttadir** (`IP4(a,b,c,d)`).
* **ARP:** 8 girisli onbellek (10 dk omur), 250 ms'de bir yeniden istek, ag disi hedefler icin ag gecidi. ARP beklemesi yalniz kesmeler aciksa yapilir.
* **ICMP:** giden `ping` (RTT), gelen echo'ya yanit (saglama dogrulanir, yayina yanit verilmez).
* **UDP:** 16 soketlik tablo, soket basina 8 datagramlik kuyruk; uzunluk/saglama dogrulanir; `connect` es filtresi.
* **DHCP:** Discover/Offer/Request/Ack, NAK isleme, secenek ayristirma (paket sinirlarina karsi korumali); acilista arka planda otomatik
  (basarisizsa varsayilan statik 10.0.2.15/24 korunur); T1'de unicast, T2'de yayin kira yenileme; kira dolunca yapilandirma birakilip yeniden edinilir.
  Yenileme (zorlanmis) test edilir; T1/T2/kira-dolma zaman yollari dogal sure beklenerek test EDILMEZ.
* **DNS:** A kaydi, UDP/53, CNAME atlanir, sikistirma isaretcileri takip edilmez (dongu yok), NXDOMAIN ayirt edilir; sunucu DHCP'den gelir.
* **TCP istemcisi:** `connect`, sirali guvenilir iletim, akis kontrolu, geri-N yeniden iletim (RTO 300 ms'den ustel, 4 sn'ye kadar, 8 deneme), FIN
  kapanisi, RST. 8 baglanti, yon basina 16 KB tampon, gonderim penceresi 4 MSS (1460). Sinirlar: **listen/accept yok**; pencere olcekleme/SACK/zaman damgasi yok;
  sirasiz gelen veri atilir; RTT olcumu ve hizli yeniden iletim yok; TIME_WAIT 2 sn; ICMP hata iletileri islenmez; IPv6 yok.
* Komutlar: `ping`, `netinfo`, `netcfg`, `dhcp`, `nslookup`, `netstat`; ring 3'te `ifconfig`, `nslookup`, `nc`.

## NVMe
PCI 01:08:02 taranir (BAR 64 bit olabilir, `mmio_map` ile eslenir). UEFI'nin acik biraktigi kontrolcu once kapatilir (CC.EN=0). Admin + 1 I/O kuyrugu
(derinlik <= 32), namespace 1, yalniz 512 B sektor, polling. Her komutun zaman asimi vardir (CAP.TO'dan, en az 2 sn); asilirsa ya da CSTS.CFS
gorulurse aygit "olu" isaretlenir ve sonraki istekler beklemeden hata doner. Veri 64 KB'lik sayfa hizali bounce tamponundan gecer (DMA ilk 4 GB'ta).
Her yazmadan sonra Flush gonderilir. Sinirlar: tek namespace, kesme yok, MPSMIN != 0 ve 512 B disi sektor desteklenmez.

## ACPI, APIC bilgisi, IOMMU
RSDP (UEFI'den) -> XSDT/RSDT -> FADT (kapatma/yeniden baslatma), MADT, DMAR, IVRS (`acpi_find_table`: uzunluk ve saglama dogrulanir).
`acpi` komutu: CPU sayisi, Local APIC adresi, I/O APIC'ler, ISA IRQ yonlendirme istisnalari. **Yalnizca raporlanir:** kernel hala 8259 PIC ve PIT kullanir.

`iommu` komutu: **Intel VT-d** icin DMAR tablosundan DRHD birimleri okunur ve her birimin VER/CAP/ECAP/GSTS yazmaclari (salt okuma) gosterilir
(DMA remapping ve kesme remapping'in firmware tarafindan acik birakilip birakilmadigi dahil; aciksa surucu DMA'sinin engellenebilecegi uyarisi verilir);
RMRR sayisi ve adres genisligi raporlanir. **AMD-Vi** icin IVRS'deki IVHD bloklari sayilir. **DMA remapping etkinlestirilmez.**

## Guvenlik ozellikleri
* **SMEP / SMAP:** CPUID.7.0:EBX[7]/[20] bildiriyorsa CR4.20/CR4.21 acilir. Kernel kullanici gostericilerini hic dogrudan kullanmaz
  (`copy_from_user`/`copy_to_user` sayfa tablosunu yurutup fiziksel adresle kopyalar), bu yuzden `stac/clac` gerekmez (`cpu_stac/cpu_clac` hazir);
  SMAP yanlislikla dogrudan erisimi #PF ile yakalar ve panik ekrani "SMAP IHLALI" teshisi verir. SFMASK syscall girisinde AC'yi temizler.
* **Yigin canary'si:** kernel ve ring 3 programlari `-fstack-protector-strong -mstack-protector-guard=global` ile derlenir (x86_64'te varsayilan
  `%fs:0x28` TLS gerektirir; kernelde FS tabani yok). Kernel degeri `entry.S`'de `kernel_main`'den once RDRAND/TSC ile rastgele atanir (dusuk bayt 0);
  ring 3 degerini `_start`'ta `SYS_RANDOM`'dan alir. Tasma: kernelde panik ("YIGIN TASMASI"), ring 3'te surec -1 ile sonlandirilir. Bootloader (UEFI) korumasizdir.
* NX/WP, kernel kodu R-X, `.rodata` R--, kullanici adres uzayi ayri PML4, kullanici sayfalarina supervisor kodu calistirma yok (SMEP).

## Masaustu uygulamalari
Terminal (kabuk), Dosyalar (gezinme, yeni klasor/dosya, sil, yeniden adlandir, program calistirma), Not Defteri (Ctrl+S kaydet, Ctrl+O ac, Ctrl+N yeni),
Sistem Izleyici, Hesap Makinesi, Hakkinda.

## Dosya sistemi dogrulamasi
FAT yazma kodu, Linux'un kendi araclariyla capraz dogrulanir: DEOS ile dosya yazdiktan sonra `fsck.fat -n data.img` temiz cikmali,
`mdir -/ -i data.img ::` ve `mtype` ayni agaci ve icerigi gostermelidir.

## Testler
`hwtest` / `selftest` (`selftest_hw.c`, `selftest.c`) ve `user/libtest.c`, `user/nettest.c`, `user/smash.c`:

* ACPI/MADT/tablo arama, CPU korumalari (SMEP/SMAP/PAE), TR Q + US klavye tablolari (PS/2 ve USB HID), IOMMU (DMAR/IVRS).
* ELF yukleyici: gecerli ve ~25 bozuk girdi + sayfa sizintisi.
* Ag birim testleri: saglama toplami, IP ayristirma, **13 bozuk IPv4/UDP cerceve turu**, kuyruk tasmasi, `connect` filtresi, DHCP ayristirici
  (bozuk secenekler dahil), DNS sorgu olusturma ve 15+ yanit senaryosu (sikistirma dongusu, kesik veri, NXDOMAIN, CNAME...).
* Ag canli testleri: DHCP, 24 ping (TX halkasi turu), kira yenileme, UDP yanki (1-1472 bayt), TCP yanki (100000 bayt), kapanis ve kaynak iadesi,
  reddedilen/yanitsiz baglanti, DNS uctan uca.
* NVMe: okuma/yazma (hizasiz tampon, 128 sektoru asan istek, sinir kontrolleri).
* USB hub: topoloji, hub arkasinda klavye + depolama, route string, hub arkasindaki diskten gercek sektor okuma.
* Ring 3: `libtest` (94 kontrol: printf bicimleri, malloc/realloc, stdio, ortam, pipe, `sh -c` boru hatti/yonlendirme/degisken), `nettest` (18 kontrol:
  soket API, hata kodlari, UDP/TCP uctan uca, acik soketle cikis), `smash` (ring 3 canary).

## Kaynak duzeni
| Alan | Dosyalar |
|---|---|
| Onyukleme | `bootloader.c` (UEFI), `efi.h`, `bootinfo.h` |
| Yapilandirma / temel | `config.h`, `entry.S`, `kernel.c`, `kernel.ld`, `types.h`, `cpu.h`, `io.h`, `string.c`, `kprintf.c`, `serial.c`, `rand.c` |
| Kesmeler | `gdt.c` (GDT+TSS+IST), `idt.c`, `isr_stubs.S`, `irq*.{c,S}`, `pic.c`, `timer.c` (PIT) |
| Bellek | `pmm.c`, `heap.c`, `paging.c` (4 seviyeli, NX/WP/PAT, MMIO) |
| Surecler | `thread.c` + `switch.S`, `process.c` (ortam dahil), `elf.c`, `syscall.c` + `usermode.S` |
| Platform | `acpi.c` (RSDP/FADT/MADT, tablo arama), `iommu.c` (DMAR/IVRS), `pci.c` |
| Depolama | `ahci.c`, `nvme.c`, `blk.c`, `fat.c`, `fs.c` |
| Dosya nesneleri | `file.c` (dosya/dizin/konsol), `fileobj.c` (pipe ve soket) |
| Ag | `net.c` (e1000, ARP, IPv4, ICMP), `udp.c`, `dhcp.c`, `dns.c`, `tcp.c`, `net.h` (genel API), `net_priv.h` |
| Girdi / USB | `keyboard.c`, `mouse.c`, `rtc.c`, `xhci.c` (hub dahil), `usb_hid.c`, `usb_msc.c`, `usb.h` |
| Grafik / arayuz | `console.c`, `gfx.c`, `font8x16.h` (`gen_font.py` uretir), `wm.c`, `apps.c` |
| Kullanici | `user/` (programlar, `ulib.c`, `ustdlib.c`, `ustdio.c`, `ulib.h`, `user.ld`), `syscall_abi.h`, `progs.h` |
| Testler | `selftest.c`, `selftest_hw.c`, `ci_test.sh`, `ci/echo_server.py` |
| Betikler | `build.sh`, `make_image.sh`, `test_in_qemu.sh`, `test_debug.sh`, `ci_test.sh`, `clean.sh` |
| Uretilen (depoya girmez, `.gitignore`) | `*.o`, `kernel.elf`, `KERNEL.BIN`, `BOOTX64.EFI`, `build_user/`, `progs_table.c`, `*.img`, `OVMF_VARS_*.fd`, `ci_*.log` |

## Tasarim notlari
* Kernel `0x400000`'a yuklenir; bolumler 4 KB hizali: `.text` R-X, `.rodata` R--, `.data/.bss` RW-.
* Her surecin kendi PML4'u vardir; PML4[0] (kernel, birebir esleme) ortaktir ve supervisor-only'dir. Kullanici adres uzayi `0x8000000000` ve uzeridir.
* Syscall: `rax` numara; `rdi, rsi, rdx, r10, r8` arguman. Hata kodlari negatif (`syscall_abi.h`).
* AHCI/NVMe/e1000: DMA tamponlari ilk 4 GB'ta; polling. Zaman: PIT `TIMER_HZ` (100 Hz, `config.h`); zaman asimlari tick'e cevrilir.
* Ag kritik bolgeleri kisa tutulur (kesmeler kapali); kritik bolgede `ip_output` hic uyumaz (ARP yoksa istek yollayip paketi atar, TCP yeniden dener).
* Kernel, bir thread stack'inin koruma sayfasini acarken henuz bolunmemis 2 MB'lik bolge icin tembel bir sayfa tablosu sayfasi ayirabilir (kalici, sizinti
  degil); bellek testleri bu yuzden `bos sayfa + tablo sayfalari` toplamini karsilastirir.
* FAT: okuma + yazma (dosya/dizin olusturma, silme, yeniden adlandirma, uzun adlar). Her yazma aninda diske gider (AHCI/NVMe flush); FSInfo guncellenir.
  Bir global kilit (`fs.c`) GUI/kabuk/sureclerin es zamanli erisimini korur.

## Bilinen eksikler
| Eksik | Ayrinti |
|---|---|
| APIC / SMP | Yok; PS/2 ve 8259 PIC gerektirmeyen yeni donanimlarda kesme sorunu olabilir. MADT yalniz raporlanir. |
| IOMMU etkinlestirme | Yalnizca tespit/rapor; DMA remapping, baglam tablolari, kesme remapping yok. |
| Ag kesmeleri | e1000 yoklama ile calisir (kesme/MSI yok); tek thread tum RX'i isler. |
| TCP sunucusu | `listen/accept` yok. Ayrica pencere olcekleme, SACK, RTT olcumu, hizli yeniden iletim yok; sirasiz veri atilir. |
| Ag kapsami | IPv6, IP parcalama/birlestirme, ICMP hata iletileri, DHCP T1/T2/kira-dolma yollarinin dogal sure testi yok. |
| USB | USB 3.x hub, hub basina 8'den fazla port, EHCI/OHCI/UHCI yok; QEMU'da calisirken takilan cihaz tanimiyor. |
| Depolama | FAT disinda dosya sistemi yok; yazma onbellegi yok (her yazma aninda diske, guvenli ama yavas); izin/sahiplik yok. |
| Kullanici alani | Sinyal yok (surec yalniz `kill`/hata ile biter), kayan nokta/locale/setjmp yok, TCP/UDP'de dinleme yok, `dup2` yok (yonlendirme `spawn` fdmap ile). |
| DMA siniri | AHCI/NVMe/e1000 DMA tamponlari ilk 4 GB'ta olmak zorundadir. |
| Secure Boot | Bootloader imzasizdir; gercek makinede Secure Boot kapatilmalidir. Bootloader yigin canary'sizdir. |
| Gercek donanim | Hicbir surucu gercek makinede dogrulanmadi (yalniz QEMU/OVMF). |
| TR klavye | AltGr yok; noktalama tam TR Q degildir (bkz. `keyboard.c`). |

## Surum gecmisi
### Pr-4
* **Guvenlik:** SMAP etkinlestirildi (CR4.21; panik teshisi; `crash smap` ve CI asamasi); yigin canary'si kernel + ring 3'te gercekten acik
  (`-fstack-protector-strong -mstack-protector-guard=global`, giriste rastgele deger, `SYS_RANDOM`, `crash canary` ve CI asamasi, `smash` programi);
  `copy_*_user` yeniden yazildi (sayfa sayfa, `const` duzgun, dizge kopyalama bayt basina sayfa yurumeden); `rand.c`.
* **IOMMU:** sahte PCI-sinifi tahmini kaldirildi; ACPI DMAR (Intel VT-d, birim yazmaclari) ve IVRS (AMD-Vi) tablolarindan gercek tespit; `iommu` komutu; testler QEMU `intel-iommu` ile.
* **Ag yigini yeniden yazildi** (`net.c` + `udp.c`, `dhcp.c`, `dns.c`, `tcp.c`): IPv4/UDP/TCP/ICMP girisinde tam dogrulama (eski surumde UDP uzunlugu cerceve
  sinirini asabiliyordu); IP adresi bayt sirasi duzeltildi (varsayilan IP eskiden ag uzerinde 15.2.0.10 gidiyordu); asenkron coklu-tamponlu TX; coklu UDP soketi;
  tam DHCP istemcisi (kira yenileme, acilista otomatik); DNS istemcisi; TCP istemcisi; ag gecidi/maske/DNS yapilandirmasi; `dhcp`, `nslookup`, `netstat`.
* **USB 2.0 hub destegi** (route string, TT, port yoklama, hub arkasinda hub, cikarma).
* **Kullanici alani:** C kutuphanesi (malloc, stdio, printf ailesi, stdlib, ortam), pipe, soket syscall'lari, `getrandom`, PATH ile program arama, kabukta boru hatti/yonlendirme/
  degisken, yeni araclar (`grep head env ifconfig nslookup nc`), `cat`/`wc` stdin'den okur.
* **Testler:** 435 kontrol + `libtest` (94) + `nettest` (18); `ci_test.sh` dort asamali; host tarafi yanki/DNS sunucusu; USB hub ve ikinci USB diski.
  Test altyapisinda duzeltme: bellek sizinti testleri tembel kernel sayfa tablosu sayfalarini hesaba katmiyordu (zamanlamaya bagli rastgele basarisizlik).
* **Surum sistemi:** eski numarali surum sistemi kaldirildi (`Pr-N`); `adim3.c` -> `bootloader.c`; surum dizgisi bootloader, kernel ve masaustunde `DEOS_VERSION`'dan gelir.

### Pr-3 ve oncesi
* UEFI bootloader (32bpp GOP, dinamik bellek haritasi, ACPI RSDP aktarimi, ExitBootServices yeniden denemesi); preemptive kernel, sayfalama, ring 3, ELF, syscall,
  AHCI + FAT, USB (xHCI, HID, depolama), PS/2 ve USB klavye/fare, Turkce Q klavye, masaustu, e1000 + ARP/IPv4/ICMP, ACPI kapatma/yeniden baslatma.
* Pr-3'te: **NVMe surucusu bastan yazildi** (onceki surum QEMU'da bile baslamiyordu: CC alanlari, SQE yapisi, kuyruk komutlari, faz biti, zaman asimlari hatalariydi),
  ACPI MADT ayristirma, e1000 TX halkasi hatasi (yalniz ilk cerceve gidiyordu), TR klavyede I/İ hatasi, `selftest_hw.c` + `ci_test.sh` + NVMe/e1000'li test betikleri,
  README'nin gercek durumla uyumlu yeniden yazimi, `GERCEK_DONANIM.md`.

## Yol haritasi (Pr-5 adaylari)
1. APIC/IOAPIC'e gecis ve SMP; kesme tabanli ag/NVMe.
2. IOMMU'yu etkinlestirme (VT-d baglam tablolari, DMA koruma).
3. TCP: `listen/accept`, RTT olcumu, hizli yeniden iletim; ICMP hata iletileri; IPv6.
4. USB 3.x hub, EHCI; QEMU'da calisirken cihaz takma sorunu.
5. FAT yazma onbellegi (veri guvenligiyle birlikte), ek dosya sistemleri, izin/sahiplik, sinyaller.
6. Gercek donanim testleri (`GERCEK_DONANIM.md` tablosunu doldurun).

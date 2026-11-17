DEOS dosya sistemi
==================
Bu dosya DEOS tarafindan diskten (AHCI/SATA, NVMe ya da USB uzerinden, FAT dosya sistemiyle) okunuyor.

Cekirdek kabugu: ls, cd, pwd, cat, disks, mounts, lspci, lsusb, run <program>, ping, dhcp, nslookup, netinfo, netstat
Ring 3 kabugu:   run sh   ('help' yazin): boru hatti (a | b), yonlendirme (< > >>), $DEGISKEN, export, PATH
Programlar /bin altinda: ls /bin   ve   run hello
Ag araclari: ifconfig, nslookup <ad>, nc [-u] <ana_makine> <port>
Metin araclari: cat, grep, head, wc, hexdump, env, echo
Ikinci satirdan okunan not.
Uzun dosya adlari (LFN) da desteklenir: ls /docs

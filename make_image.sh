#!/usr/bin/env bash
# DEOS - test disk imajlarini hazirlar (build.sh'tan sonra calistirin):
#   esp.img  : onyukleme diski (FAT32, UEFI bootloader + kernel + /bin + ornek dosyalar) -> kernel'de "sd0"
#   nvme.img : NVMe deneme diski (QEMU -device nvme)                                       -> kernel'de "nvme0"
#   data.img : kalici veri diski (FAT32, bir kez olusturulur, her calistirmada KORUNUR)   -> kernel'de "sd1"
set -e
cd "$(dirname "$0")"

rm -f esp.img
dd if=/dev/zero of=esp.img bs=1M count=64 2>/dev/null
mformat -i esp.img -F ::
mmd -i esp.img ::/EFI ::/EFI/BOOT ::/BIN
mcopy -i esp.img BOOTX64.EFI ::/EFI/BOOT/BOOTX64.EFI
mcopy -i esp.img KERNEL.BIN ::/EFI/BOOT/KERNEL.BIN
mcopy -i esp.img build_user/*.elf ::/BIN/
mcopy -i esp.img -s rootfs/* ::/

if [ ! -f data.img ]; then
    dd if=/dev/zero of=data.img bs=1M count=32 2>/dev/null
    mformat -i data.img -F -v DEOSDATA ::
    echo "Bu dosya ikinci diskte (data.img): kalici." > /tmp/_merhaba.txt
    mcopy -i data.img /tmp/_merhaba.txt ::/MERHABA.TXT
fi

# Hub arkasindaki ikinci USB deneme diski (QEMU: usb-hub + usb-storage port=4.2): FAT32.
if [ ! -f usb2.img ]; then
    dd if=/dev/zero of=usb2.img bs=1M count=16 2>/dev/null
    mformat -i usb2.img -F -v DEOSHUB ::
    echo "Bu dosya hub arkasindaki USB diskte (usb2.img)." > /tmp/_usb2.txt
    mcopy -i usb2.img /tmp/_usb2.txt ::/HUB.TXT
fi

# USB deneme diski (QEMU'da "-device usb-storage" ile takilir): FAT32, bir kez olusturulur, korunur.
if [ ! -f usb.img ]; then
    dd if=/dev/zero of=usb.img bs=1M count=32 2>/dev/null
    mformat -i usb.img -F -v DEOSUSB ::
    echo "Bu dosya USB diskte (usb.img)." > /tmp/_usb.txt
    mcopy -i usb.img /tmp/_usb.txt ::/USB.TXT
fi

# NVMe deneme diski (QEMU'da "-device nvme" ile takilir): FAT32, bir kez olusturulur, korunur.
if [ ! -f nvme.img ]; then
    dd if=/dev/zero of=nvme.img bs=1M count=32 2>/dev/null
    mformat -i nvme.img -F -v DEOSNVME ::
    echo "Bu dosya NVMe diskte (nvme.img)." > /tmp/_nvme.txt
    mcopy -i nvme.img /tmp/_nvme.txt ::/NVME.TXT
fi

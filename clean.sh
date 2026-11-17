#!/usr/bin/env bash
# DEOS - uretilmis dosyalari temizler (kaynak kodu ve rootfs'e dokunmaz).
cd "$(dirname "$0")"
rm -rf build_user *.o kernel.elf KERNEL.BIN BOOTX64.EFI progs_table.c esp.img OVMF_VARS_local.fd OVMF_VARS_ci.fd ci_serial.log
echo "Temizlendi. (Kalici disk imajlari data.img / usb.img / nvme.img korundu; silmek icin elle kaldirin.)"

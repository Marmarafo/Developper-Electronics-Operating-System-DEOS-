#!/usr/bin/env bash
set -e
cd "$(dirname "$0")"
OVMF_CODE="/usr/share/OVMF/OVMF_CODE_4M.fd"
OVMF_VARS="/usr/share/OVMF/OVMF_VARS_4M.fd"

if [ ! -f "$OVMF_CODE" ]; then echo "HATA: $OVMF_CODE bulunamadi."; exit 1; fi
if [ ! -f "BOOTX64.EFI" ] || [ ! -f "KERNEL.BIN" ]; then echo "HATA: once ./build.sh calistirin."; exit 1; fi

pkill -9 qemu-system-x86_64 2>/dev/null || true

bash ./make_image.sh
cp "$OVMF_VARS" ./OVMF_VARS_local.fd

qemu-system-x86_64 \
    -machine q35 -m 512 \
    -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
    -drive if=pflash,format=raw,file=./OVMF_VARS_local.fd \
    -drive format=raw,file=esp.img \
    -drive format=raw,file=data.img \
    -display vnc=127.0.0.1:1 -monitor none \
    -rtc base=localtime \
    -serial stdio \
    -net none

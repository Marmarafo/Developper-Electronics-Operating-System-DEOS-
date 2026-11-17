#!/usr/bin/env bash
# DEOS - otomatik test (CI). Birkac asamada kernel'i uygun bayraklarla derler, QEMU'da basliksiz (headless)
# acar ve seri porttan sonucu okur:
#   1) selftest-q64   : BOOT_SELFTEST=1, varsayilan CPU (qemu64: SMEP/SMAP yok)
#   2) selftest-max   : BOOT_SELFTEST=1, CPU=max (SMEP + SMAP acik)
#   3) crash-smap     : BOOT_CRASH=smap   -> kernel, kullanici sayfasini stac'siz okur; "SMAP IHLALI" paniği beklenir
#   4) crash-canary   : BOOT_CRASH=canary -> yerel tampon tasar; "YIGIN TASMASI" paniği beklenir
# Aygitlar: xHCI (klavye/fare/USB disk) + AHCI + NVMe + e1000 (+ Intel IOMMU). Ag testleri icin sunucu betikleri
# (ci/echo_server.py) host'ta baslatilir; guest bunlara slirp uzerinden 10.0.2.2 ile baglanir.
# Cikis kodu: 0 = hepsi gecti, 1 = bir asama basarisiz, 3 = ortam eksik.
# Ortam degiskenleri: ACCEL=kvm, TIMEOUT=sn (asama basina), STAGES="selftest-q64 ..." (alt kume).
# Gerekenler: x86_64-linux-gnu-gcc/ld, qemu-system-x86_64, OVMF, mtools, python3.
set -u
cd "$(dirname "$0")"
OVMF_CODE="${OVMF_CODE:-/usr/share/OVMF/OVMF_CODE_4M.fd}"
OVMF_VARS="${OVMF_VARS:-/usr/share/OVMF/OVMF_VARS_4M.fd}"
TIMEOUT="${TIMEOUT:-300}"
ACCEL="${ACCEL:-tcg}"
STAGES="${STAGES:-selftest-q64 selftest-max crash-smap crash-canary}"
ECHO_PORT="${ECHO_PORT:-47007}"

for t in qemu-system-x86_64 mformat mcopy x86_64-linux-gnu-gcc python3; do
    command -v "$t" >/dev/null || { echo "HATA: '$t' bulunamadi."; exit 3; }
done
[ -f "$OVMF_CODE" ] || { echo "HATA: $OVMF_CODE bulunamadi."; exit 3; }

SERVER_PID=""
cleanup() {
    [ -n "$SERVER_PID" ] && kill "$SERVER_PID" 2>/dev/null
    pkill -f "qemu-system-x86_64.*OVMF_VARS_ci" 2>/dev/null
    ./build.sh >/dev/null 2>&1 && ./make_image.sh >/dev/null 2>&1      # normal (testsiz) derlemeyi geri yukle
}
trap cleanup EXIT

clean_log() { sed 's/\x1b\[[0-9;]*m//g' "$1" 2>/dev/null | tr -d '\r'; }

# boot_qemu <log> <cpu> <bitis-regex> : QEMU'yu arka planda acar, log'da regex gorunene veya QEMU kapanana
# (ya da TIMEOUT dolana) kadar bekler, sonra kapatir.
boot_qemu() {
    local log="$1" cpu="$2" stop="$3"
    cp "$OVMF_VARS" ./OVMF_VARS_ci.fd
    rm -f "$log"
    qemu-system-x86_64 \
        -machine q35 -m 512 -accel "$ACCEL" -cpu "$cpu" \
        -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
        -drive if=pflash,format=raw,file=./OVMF_VARS_ci.fd \
        -drive format=raw,file=esp.img \
        -drive format=raw,file=data.img \
        -device qemu-xhci,id=xhci -device usb-kbd,bus=xhci.0 -device usb-mouse,bus=xhci.0 \
        -drive if=none,id=usbdisk,format=raw,file=usb.img -device usb-storage,bus=xhci.0,drive=usbdisk \
        -device usb-hub,bus=xhci.0,port=4 -device usb-kbd,bus=xhci.0,port=4.1 \
        -drive if=none,id=usbdisk2,format=raw,file=usb2.img -device usb-storage,bus=xhci.0,port=4.2,drive=usbdisk2 \
        -drive if=none,id=nvm,format=raw,file=nvme.img -device nvme,drive=nvm,serial=deos0 \
        -device intel-iommu \
        -nic user,model=e1000 \
        -display none -serial file:"$log" -no-reboot >/dev/null 2>&1 &
    local qpid=$! waited=0
    while kill -0 "$qpid" 2>/dev/null && [ "$waited" -lt "$TIMEOUT" ]; do
        if clean_log "$log" | grep -qE "$stop"; then sleep 2; break; fi
        sleep 1; waited=$((waited + 1))
    done
    kill "$qpid" 2>/dev/null; wait "$qpid" 2>/dev/null
}

fresh_disks() { rm -f data.img usb.img usb2.img nvme.img; ./make_image.sh >/dev/null 2>&1; }

FAILED=0; SUMMARY=""
record() { # <ad> <0|1> <ayrinti>
    if [ "$2" = 0 ]; then SUMMARY="$SUMMARY\n  [GECTI]  $1  $3"; else SUMMARY="$SUMMARY\n  [KALDI]  $1  $3"; FAILED=1; fi
}

run_selftest() { # <ad> <cpu>
    local name="$1" cpu="$2" log="ci_$1.log"
    echo "--- [$name] derleniyor (BOOT_SELFTEST=1) ---"
    EXTRA_CFLAGS="-DCI_ECHO_PORT=$ECHO_PORT" BOOT_SELFTEST=1 ./build.sh >/tmp/ci_build.log 2>&1 || { tail -20 /tmp/ci_build.log; record "$name" 1 "derleme basarisiz"; return; }
    grep -q "warning" /tmp/ci_build.log && { echo "UYARI: derleme uyarilari:"; grep warning /tmp/ci_build.log | head; }
    fresh_disks
    python3 ci/echo_server.py "$ECHO_PORT" >/tmp/ci_echo.log 2>&1 &
    SERVER_PID=$!
    echo "--- [$name] QEMU (cpu=$cpu) ---"
    boot_qemu "$log" "$cpu" "\[SELFTEST\] SONUC"
    kill "$SERVER_PID" 2>/dev/null; SERVER_PID=""
    local out; out="$(clean_log "$log")"
    echo "$out" | grep -E "^\s*\[FAIL\]|\[SELFTEST\]" || true
    if echo "$out" | grep -q "\[SELFTEST\] SONUC: GECTI"; then record "$name" 0 "($(echo "$out" | grep -c '\[PASS\]') kontrol)"
    elif echo "$out" | grep -q "\[SELFTEST\] SONUC: KALDI"; then record "$name" 1 "basarisiz test var (log: $log)"
    else record "$name" 1 "sonuc alinamadi (log: $log)"; tail -8 "$log" | sed 's/\x1b\[[0-9;]*m//g'; fi
}

run_crash() { # <ad> <tur> <cpu> <beklenen-metin>
    local name="$1" kind="$2" cpu="$3" want="$4" log="ci_$1.log"
    echo "--- [$name] derleniyor (BOOT_CRASH=$kind) ---"
    BOOT_CRASH="$kind" ./build.sh >/tmp/ci_build.log 2>&1 || { tail -20 /tmp/ci_build.log; record "$name" 1 "derleme basarisiz"; return; }
    ./make_image.sh >/dev/null 2>&1
    echo "--- [$name] QEMU (cpu=$cpu) ---"
    boot_qemu "$log" "$cpu" "KERNEL PANIC|\[CRASHTEST\] PANIK OLMADI|SMAP etkin degil"
    local out; out="$(clean_log "$log")"
    echo "$out" | grep -E "CRASHTEST|PANIC|Teshis|TASMASI|SMAP" | head -6
    if echo "$out" | grep -q "$want"; then record "$name" 0 "panik beklenen nedenle olustu"
    elif echo "$out" | grep -q "SMAP etkin degil"; then record "$name" 1 "islemci SMAP bildirmiyor (CPU=max kullanin)"
    elif echo "$out" | grep -q "PANIK OLMADI"; then record "$name" 1 "KORUMA CALISMADI: panik olusmadi"
    else record "$name" 1 "beklenen cikti yok (log: $log)"; fi
}

for st in $STAGES; do
    case "$st" in
        selftest-q64) run_selftest selftest-q64 qemu64 ;;
        selftest-max) run_selftest selftest-max max ;;
        crash-smap)   run_crash crash-smap smap max "SMAP IHLALI" ;;
        crash-canary) run_crash crash-canary canary qemu64 "YIGIN TASMASI" ;;
        *) echo "bilinmeyen asama: $st"; FAILED=1 ;;
    esac
done

echo; echo "=== CI OZETI ==="; printf "$SUMMARY\n"
if [ "$FAILED" = 0 ]; then echo "=== CI: HEPSI GECTI ==="; exit 0; else echo "=== CI: BASARISIZ ASAMA VAR ==="; exit 1; fi

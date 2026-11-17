#!/usr/bin/env bash
# DEOS derleme scripti: bootloader + kullanici programlari (ELF) + kernel
# Kullanim:  ./build.sh                    normal derleme
#            BOOT_SELFTEST=1 ./build.sh    acilista tum testleri calistiran derleme (ci_test.sh bunu kullanir)
#            BOOT_CRASH=smap ./build.sh    acilista kasitli hata denemesi (smap | canary | ...; ci_test.sh bunu kullanir)
set -e
cd "$(dirname "$0")"

echo "--- Bootloader derleniyor (BOOTX64.EFI) ---"
x86_64-linux-gnu-gcc -c bootloader.c -o bootloader.o \
    -ffreestanding -fno-stack-protector -fno-stack-check -fshort-wchar \
    -mno-red-zone -maccumulate-outgoing-args -fpic -Wall -Wextra

x86_64-linux-gnu-ld bootloader.o -o BOOTX64.EFI \
    -nostdlib --subsystem 10 -e efi_main -m i386pep

x86_64-linux-gnu-objcopy -R .comment -R .note.gnu.property BOOTX64.EFI BOOTX64.EFI

# ---------------------------------------------------------------------------
echo "--- Kullanici programlari derleniyor (ELF) ---"
UCFLAGS="-ffreestanding -fPIE -fvisibility=hidden -fstack-protector-strong -mstack-protector-guard=global -mno-red-zone -mgeneral-regs-only \
-fcf-protection=none -fno-asynchronous-unwind-tables -ffunction-sections -fdata-sections -O1 -Wall -Wextra -I."  # -fPIE: kod 4 GB ustunde, RIP-goreli adresleme sart
EMBED="hello ticker heaptest reverse badboy fault"          # kernel icine gomulur
DISK="sh ls cat echo mkdir rm touch cp mv wc hexdump systest grep head env ifconfig nslookup nc libtest nettest smash"  # sadece diskte (/BIN)
PROGS="$EMBED $DISK"

rm -rf build_user && mkdir build_user
x86_64-linux-gnu-gcc -c user/ulib.c -o build_user/ulib.o $UCFLAGS
for l in ustdlib ustdio; do x86_64-linux-gnu-gcc -c user/$l.c -o build_user/$l.o $UCFLAGS; done
ULIBS="build_user/ulib.o build_user/ustdlib.o build_user/ustdio.o"
for p in $PROGS; do
    x86_64-linux-gnu-gcc -c user/$p.c -o build_user/$p.o $UCFLAGS
    x86_64-linux-gnu-ld -T user/user.ld -nostdlib -z max-page-size=4096 -z noexecstack \
        --gc-sections -o build_user/$p.elf build_user/$p.o $ULIBS
    x86_64-linux-gnu-strip --strip-all build_user/$p.elf
done
for p in $EMBED; do
    # ELF'i kernel'e gomulecek nesne dosyasina cevir (sembol: _binary_<ad>_elf_start/_end)
    (cd build_user && x86_64-linux-gnu-objcopy -I binary -O elf64-x86-64 -B i386:x86-64 \
        --rename-section .data=.rodata,alloc,load,readonly,data,contents $p.elf ${p}_elf.o)
done

# Gomulu program tablosu
{
    echo '#include "progs.h"'
    for p in $EMBED; do
        echo "extern const unsigned char _binary_${p}_elf_start[], _binary_${p}_elf_end[];"
    done
    echo 'const embedded_prog_t g_progs[] = {'
    for p in $EMBED; do
        echo "    { \"$p\", _binary_${p}_elf_start, _binary_${p}_elf_end },"
    done
    echo '    { 0, 0, 0 }'
    echo '};'
} > progs_table.c

# ---------------------------------------------------------------------------
echo "--- Kernel derleniyor (KERNEL.BIN) ---"
CFLAGS="-ffreestanding -fstack-protector-strong -mstack-protector-guard=global -fno-stack-check -fno-pic -fno-pie \
-mno-red-zone -mgeneral-regs-only -Wall -Wextra"
[ "${BOOT_SELFTEST:-0}" = "1" ] && CFLAGS="$CFLAGS -DBOOT_SELFTEST=1"
[ -n "${EXTRA_CFLAGS:-}" ] && CFLAGS="$CFLAGS $EXTRA_CFLAGS"
[ -n "${BOOT_CRASH:-}" ] && CFLAGS="$CFLAGS -DBOOT_CRASH=\"${BOOT_CRASH}\""

C_SOURCES="kernel gdt idt pic irq timer keyboard console kprintf string serial rand pmm heap paging thread elf process syscall selftest selftest_hw mouse rtc gfx wm apps pci ahci nvme acpi net udp dhcp dns tcp iommu blk fat fs file fileobj xhci usb_hid usb_msc progs_table"
S_SOURCES="entry gdt_flush idt_load isr_stubs irq_stubs switch usermode"

OBJS=""
for f in $C_SOURCES; do
    OPT=""
    case $f in gfx|wm|apps) OPT="-O2" ;; esac      # grafik/masaustu kodu hizli olsun
    x86_64-linux-gnu-gcc -c $f.c -o $f.o $CFLAGS $OPT
    OBJS="$OBJS $f.o"
done
for f in $S_SOURCES; do
    x86_64-linux-gnu-gcc -c $f.S -o $f.o
    OBJS="$OBJS $f.o"
done
for p in $EMBED; do OBJS="$OBJS build_user/${p}_elf.o"; done

x86_64-linux-gnu-ld -T kernel.ld -nostdlib -z noexecstack -o kernel.elf $OBJS

# .bss'i de dosyaya dahil et: boylece (1) bootloader tum kernel bellegini
# (bss + stack dahil) AllocatePages ile ayirir, (2) bss SIFIRLANMIS gelir.
x86_64-linux-gnu-objcopy -O binary \
    --set-section-flags .bss=alloc,load,contents kernel.elf KERNEL.BIN

echo ""
echo "BASARILI: BOOTX64.EFI ve KERNEL.BIN uretildi."
file BOOTX64.EFI
ls -l KERNEL.BIN

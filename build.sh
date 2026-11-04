#!/usr/bin/env bash
# DEOS - Adim 13 derleme (depolama) scripti: bootloader + kullanici programlari (ELF) + kernel
set -e
cd "$(dirname "$0")"

echo "--- Bootloader derleniyor (BOOTX64.EFI) ---"
x86_64-linux-gnu-gcc -c adim3.c -o adim3.o \
    -ffreestanding -fno-stack-protector -fno-stack-check -fshort-wchar \
    -mno-red-zone -maccumulate-outgoing-args -fpic -Wall -Wextra

x86_64-linux-gnu-ld adim3.o -o BOOTX64.EFI \
    -nostdlib --subsystem 10 -e efi_main -m i386pep

x86_64-linux-gnu-objcopy -R .comment -R .note.gnu.property BOOTX64.EFI BOOTX64.EFI

# ---------------------------------------------------------------------------
echo "--- Kullanici programlari derleniyor (ELF) ---"
UCFLAGS="-ffreestanding -fPIE -fvisibility=hidden -fno-stack-protector -mno-red-zone -mgeneral-regs-only \
-fcf-protection=none -fno-asynchronous-unwind-tables -O1 -Wall -Wextra -I."  # -fPIE: kod 4 GB ustunde, RIP-goreli adresleme sart
PROGS="hello ticker heaptest reverse badboy fault"

rm -rf build_user && mkdir build_user
x86_64-linux-gnu-gcc -c user/ulib.c -o build_user/ulib.o $UCFLAGS
for p in $PROGS; do
    x86_64-linux-gnu-gcc -c user/$p.c -o build_user/$p.o $UCFLAGS
    x86_64-linux-gnu-ld -T user/user.ld -nostdlib -z max-page-size=4096 -z noexecstack \
        -o build_user/$p.elf build_user/$p.o build_user/ulib.o
    x86_64-linux-gnu-strip --strip-all build_user/$p.elf
    # ELF'i kernel'e gomulecek nesne dosyasina cevir (sembol: _binary_<ad>_elf_start/_end)
    (cd build_user && x86_64-linux-gnu-objcopy -I binary -O elf64-x86-64 -B i386:x86-64 \
        --rename-section .data=.rodata,alloc,load,readonly,data,contents $p.elf ${p}_elf.o)
done

# Gomulu program tablosu
{
    echo '#include "progs.h"'
    for p in $PROGS; do
        echo "extern const unsigned char _binary_${p}_elf_start[], _binary_${p}_elf_end[];"
    done
    echo 'const embedded_prog_t g_progs[] = {'
    for p in $PROGS; do
        echo "    { \"$p\", _binary_${p}_elf_start, _binary_${p}_elf_end },"
    done
    echo '    { 0, 0, 0 }'
    echo '};'
} > progs_table.c

# ---------------------------------------------------------------------------
echo "--- Kernel derleniyor (KERNEL.BIN) ---"
CFLAGS="-ffreestanding -fno-stack-protector -fno-stack-check -fno-pic -fno-pie \
-mno-red-zone -mgeneral-regs-only -Wall -Wextra"

C_SOURCES="kernel gdt idt pic irq timer keyboard console kprintf string serial pmm heap paging thread elf process syscall selftest mouse rtc gfx wm apps pci ahci blk fat fs progs_table"
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
for p in $PROGS; do OBJS="$OBJS build_user/${p}_elf.o"; done

x86_64-linux-gnu-ld -T kernel.ld -nostdlib -z noexecstack -o kernel.elf $OBJS

# .bss'i de dosyaya dahil et: boylece (1) bootloader tum kernel bellegini
# (bss + stack dahil) AllocatePages ile ayirir, (2) bss SIFIRLANMIS gelir.
x86_64-linux-gnu-objcopy -O binary \
    --set-section-flags .bss=alloc,load,contents kernel.elf KERNEL.BIN

echo ""
echo "BASARILI: BOOTX64.EFI ve KERNEL.BIN uretildi."
file BOOTX64.EFI
ls -l KERNEL.BIN

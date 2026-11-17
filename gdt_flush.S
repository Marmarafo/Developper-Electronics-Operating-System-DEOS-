# DEOS -- gdt_flush(UINT64 gdt_ptr_addr)
# rdi = GDTR yapisinin (limit+base) adresi.
# lgdt ile yeni GDT'yi CPU'ya bildirir, DS/ES/FS/GS/SS'i yeni veri
# secicisiyle (0x10) yeniler. CS'i degistirmenin uzun modda tek yolu
# "far jmp/far return" oldugu icin, donus adresini elle kod secicisiyle
# (0x08) tekrar stack'e koyup lretq (far return) ile CS'i yeniliyoruz.
.section .text
.global gdt_flush
.type gdt_flush, @function
gdt_flush:
    lgdt (%rdi)

    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    mov %ax, %ss

    pop %rdi          # cagiranin birakip gittigi donus adresi
    mov $0x08, %rax   # yeni kod segment secicisi (kernel code)
    push %rax
    push %rdi
    lretq             # far return: RIP ve CS'i birlikte yukler

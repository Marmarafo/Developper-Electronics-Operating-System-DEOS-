/* DEOS -- kernel giris noktasi. Bootloader (adim3.c) buraya, rdi = BootInfo*
   ile ziplar. UEFI'nin biraktigi stack'i KULLANMAYIZ: kendi 64KB'lik
   stack'imize gecip kernel_main'i cagiririz. */
.section .text.entry,"ax",@progbits
.global _start
.type _start, @function
_start:
    cli
    cld
    mov $0x3F8, %dx          /* hata ayiklama: seri porta 'K' (kernel'e girildi) */
    mov $'K', %al
    out %al, %dx
    lea stack_top(%rip), %rsp
    xor %ebp, %ebp
    call kernel_main
.Lhang:
    cli
    hlt
    jmp .Lhang

.section .bss
.align 4096
.global stack_guard
stack_guard:               /* KORUMA SAYFASI: paging.c bunu eslemez -> stack tasarsa page fault */
    .skip 4096
.global stack_bottom
stack_bottom:
    .skip 65536
.global stack_top
stack_top:

.section .note.GNU-stack,"",@progbits

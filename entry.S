/* DEOS -- kernel giris noktasi. Bootloader (bootloader.c) buraya, rdi = BootInfo*
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
    mov %rdi, %r12           /* BootInfo*'yu koru (cpuid/rdrand rdi'ye dokunmaz ama garanti olsun) */

    /* Yigin koruyucu degeri (kernel_main'den ONCE): TSC, varsa RDRAND ile karistirilir; dusuk bayt = 0 */
    rdtsc
    shl $32, %rdx
    or  %rdx, %rax
    mov %rax, %r8
    mov $1, %eax
    xor %ecx, %ecx
    cpuid
    bt  $30, %ecx            /* CPUID.1:ECX[30] = RDRAND */
    jnc 2f
    mov $8, %r9d
1:  rdrand %rax
    jc  3f
    dec %r9d
    jnz 1b
    jmp 2f
3:  xor %rax, %r8
2:  movabs $0x9E3779B97F4A7C15, %rax
    imul %rax, %r8
    rol $29, %r8
    imul %rax, %r8
    and $-256, %r8
    jnz 4f
    movabs $0x5A3C96E1B4D20F00, %r8
4:  mov %r8, __stack_chk_guard(%rip)
    mov %r12, %rdi
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

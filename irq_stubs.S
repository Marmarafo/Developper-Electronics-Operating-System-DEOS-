# DEOS -- donanim IRQ (PIC tarafindan vektor 32-47'ye eslenmis) icin
# stub'lar. CPU exception'larindan (isr_stubs.S) farkli olarak hicbirinde
# hata kodu bulunmaz, o yuzden hepsi ayni kaliptaki basit stub.
.section .text

.macro IRQ_STUB num
.global irq\num
irq\num:
    push $0
    push $\num
    jmp irq_common_stub
.endm

IRQ_STUB 0
IRQ_STUB 1
IRQ_STUB 2
IRQ_STUB 3
IRQ_STUB 4
IRQ_STUB 5
IRQ_STUB 6
IRQ_STUB 7
IRQ_STUB 8
IRQ_STUB 9
IRQ_STUB 10
IRQ_STUB 11
IRQ_STUB 12
IRQ_STUB 13
IRQ_STUB 14
IRQ_STUB 15

irq_common_stub:
    push %rax
    push %rbx
    push %rcx
    push %rdx
    push %rsi
    push %rdi
    push %rbp
    push %r8
    push %r9
    push %r10
    push %r11
    push %r12
    push %r13
    push %r14
    push %r15

    mov %rsp, %rdi        # isr_frame_t* -> ilk parametre (System V ABI)
    call irq_dispatch      # irq.c: dogru handler'i cagirir + PIC'e EOI gonderir

    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %r11
    pop %r10
    pop %r9
    pop %r8
    pop %rbp
    pop %rdi
    pop %rsi
    pop %rdx
    pop %rcx
    pop %rbx
    pop %rax

    add $16, %rsp
    iretq

.section .data
.global irq_stub_table
.align 8
irq_stub_table:
    .quad irq0,  irq1,  irq2,  irq3,  irq4,  irq5,  irq6,  irq7
    .quad irq8,  irq9,  irq10, irq11, irq12, irq13, irq14, irq15

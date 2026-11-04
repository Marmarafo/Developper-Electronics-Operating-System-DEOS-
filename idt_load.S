# DEOS -- idt_flush(UINT64 idt_ptr_addr)
# rdi = IDTR yapisinin (limit+base) adresi.
.section .text
.global idt_flush
.type idt_flush, @function
idt_flush:
    lidt (%rdi)
    ret

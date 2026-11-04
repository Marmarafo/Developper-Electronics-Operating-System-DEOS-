/* DEOS -- thread baglam degistirme.
   void switch_context(UINT64 *eski_rsp, UINT64 yeni_rsp)
   Callee-saved yazmaclari (System V ABI) eski thread'in stack'ine kaydeder,
   rsp'yi degistirir, yeni thread'in kaydedilmis yazmaclarini geri yukler ve
   "ret" ile yeni thread'in kaldigi yere doner. */
.section .text
.global switch_context
.type switch_context, @function
switch_context:
    push %rbp
    push %rbx
    push %r12
    push %r13
    push %r14
    push %r15
    mov %rsp, (%rdi)
    mov %rsi, %rsp
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %rbx
    pop %rbp
    ret

/* Yeni bir thread'in ILK calismasi: thread_create onun stack'ine, switch_context'in
   "ret" ile buraya dusecegi sekilde bir kare hazirlar (r12 = fonksiyon, r13 = arguman). */
.global thread_trampoline
.type thread_trampoline, @function
thread_trampoline:
    sti                       /* switch_context kesmeler kapaliyken cagrilmisti */
    mov %r13, %rdi
    call *%r12
    call thread_exit          /* fonksiyon donerse thread biter */
    ud2

.section .note.GNU-stack,"",@progbits

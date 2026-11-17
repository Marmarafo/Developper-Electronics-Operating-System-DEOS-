/* DEOS -- kullanici modu gecisleri.

   enter_user(rip, rsp, arg1, arg2): ring 3'e iretq ile atlar (CS=0x23, SS=0x1B, IF=1);
                         kullanici koduna rdi=arg1 (argc), rsi=arg2 (argv) verilir.
   syscall_entry:        "syscall" talimatinin LSTAR hedefi.

   syscall girisinde CPU stack'i DEGISTIRMEZ: rsp hala KULLANICI stack'idir,
   rcx = kullanici rip, r11 = kullanici rflags, IF=0 (SFMASK). Hemen kernel stack'ine
   geciyoruz (thread degisiminde guncellenen g_syscall_kstack). */
.section .text

.global enter_user
.type enter_user, @function
enter_user:
    cli
    xor %eax, %eax
    mov %ax, %ds              /* kullanici kodu null DS/ES ile calisir (64-bit modda gecerli) */
    mov %ax, %es
    pushq $0x1B               /* SS  = kullanici veri | RPL3 */
    pushq %rsi                /* RSP = kullanici stack */
    pushq $0x202              /* RFLAGS: IF=1 */
    pushq $0x23               /* CS  = kullanici kod | RPL3 */
    pushq %rdi                /* RIP = giris noktasi */
    mov %rdx, %rdi            /* arg1 -> rdi (argc) */
    mov %rcx, %rsi            /* arg2 -> rsi (argv) */
    iretq

.global syscall_entry
.type syscall_entry, @function
syscall_entry:
    mov %rsp, g_user_rsp_tmp(%rip)       /* IF=0: bu gecici degisken guvenli */
    mov g_syscall_kstack(%rip), %rsp
    pushq g_user_rsp_tmp(%rip)           /* kullanici rsp'sini kernel stack'ine al */
    push %rcx                            /* kullanici rip */
    push %r11                            /* kullanici rflags */
    push %rdi
    push %rsi
    push %rdx
    push %r8
    push %r9
    push %r10
    sub $8, %rsp                         /* 16 bayt hizalama */

    /* (rax, rdi, rsi, rdx, r10, r8) -> System V C cagri duzeni (rdi, rsi, rdx, rcx, r8, r9) */
    mov %r8,  %r9
    mov %r10, %r8
    mov %rdx, %rcx
    mov %rsi, %rdx
    mov %rdi, %rsi
    mov %rax, %rdi
    call syscall_dispatch                /* donus degeri rax */

    add $8, %rsp
    pop %r10
    pop %r9
    pop %r8
    pop %rdx
    pop %rsi
    pop %rdi
    pop %r11                             /* kullanici rflags */
    pop %rcx                             /* kullanici rip */
    cli                                  /* kullanici stack'ine donmeden once kesmeler KAPALI olmali */
    pop %rsp                             /* kullanici rsp */
    sysretq

.section .note.GNU-stack,"",@progbits

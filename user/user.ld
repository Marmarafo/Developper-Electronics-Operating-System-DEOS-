/* DEOS kullanici programlari: her program kendi adres uzayinda ayni adrese yuklenir.
   Bolumler 4 KB sinirinda: .text R-X, .rodata R--, .data/.bss RW- (ELF yukleyici izinleri sayfalara yansitir). */
ENTRY(_start)
SECTIONS
{
    . = 0x8000400000;
    .text : { *(.text.start) *(.text*) }
    . = ALIGN(4096);
    .rodata : { *(.rodata*) }
    . = ALIGN(4096);
    .data : { *(.data*) }
    .bss  : { *(.bss*) *(COMMON) }
    . = ALIGN(4096);
    /DISCARD/ : { *(.note*) *(.comment) *(.eh_frame*) }
}

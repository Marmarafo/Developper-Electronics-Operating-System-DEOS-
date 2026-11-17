ENTRY(_start)
SECTIONS
{
    . = 0x400000;
    __kernel_start = .;

    /* Her bolum 4 KB sinirinda baslar: sayfalama (paging) her bolume ayri
       izin verebilsin diye (.text R-X, .rodata R--, .data/.bss RW-). */
    __text_start = .;
    .text : {
        *(.text.entry)
        *(.text*)
    }
    . = ALIGN(4096);
    __text_end = .;

    __rodata_start = .;
    .rodata : { *(.rodata*) }
    . = ALIGN(4096);
    __rodata_end = .;

    __data_start = .;
    .data   : { *(.data*) }
    .bss    : { *(.bss*) *(COMMON) }
    . = ALIGN(4096);
    __data_end = .;

    __kernel_end = .;
    /DISCARD/ : { *(.note*) *(.comment) *(.eh_frame*) }
}

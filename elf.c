// DEOS -- minimal ELF64 yukleyici. Kullanici programlarini (ileride diskten de)
// ayri bir adres uzayina yerlestirir. Girdiler "guvenilmez" kabul edilir: her alan dogrulanir.
#include "types.h"
#include "elf.h"
#include "paging.h"
#include "pmm.h"
#include "string.h"
#include "syscall_abi.h"

typedef struct {
    UINT8  e_ident[16];
    UINT16 e_type, e_machine;
    UINT32 e_version;
    UINT64 e_entry, e_phoff, e_shoff;
    UINT32 e_flags;
    UINT16 e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
} Elf64_Ehdr;

typedef struct {
    UINT32 p_type, p_flags;
    UINT64 p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align;
} Elf64_Phdr;

#define PT_LOAD 1
#define PF_X 1
#define PF_W 2

int elf_load(const UINT8 *img, UINT64 size, UINT64 cr3, UINT64 *entry) {
    if (size < sizeof(Elf64_Ehdr)) return -1;
    const Elf64_Ehdr *eh = (const Elf64_Ehdr*)img;
    if (eh->e_ident[0] != 0x7F || eh->e_ident[1] != 'E' || eh->e_ident[2] != 'L' || eh->e_ident[3] != 'F') return -1;
    if (eh->e_ident[4] != 2 || eh->e_ident[5] != 1) return -1;          // 64-bit, little-endian
    if (eh->e_type != 2 || eh->e_machine != 62) return -1;              // ET_EXEC, x86-64
    if (eh->e_phentsize != sizeof(Elf64_Phdr) || eh->e_phnum == 0 || eh->e_phnum > 16) return -1;
    if (eh->e_phoff > size || size - eh->e_phoff < (UINT64)eh->e_phnum * sizeof(Elf64_Phdr)) return -1;
    if (eh->e_entry < USER_BASE + 0x1000 || eh->e_entry >= USER_HEAP_BASE) return -1;

    int loaded = 0;
    for (UINT16 i = 0; i < eh->e_phnum; i++) {
        const Elf64_Phdr *ph = (const Elf64_Phdr*)(img + eh->e_phoff) + i;
        if (ph->p_type != PT_LOAD || ph->p_memsz == 0) continue;

        if (ph->p_filesz > ph->p_memsz) return -1;
        if (ph->p_offset > size || size - ph->p_offset < ph->p_filesz) return -1;
        UINT64 vstart = ph->p_vaddr, vend = ph->p_vaddr + ph->p_memsz;
        if (vend < vstart || vstart < USER_BASE + 0x1000 || vend > USER_HEAP_BASE) return -1;

        UINT64 flags = PTE_U | ((ph->p_flags & PF_W) ? PTE_W : 0) | ((ph->p_flags & PF_X) ? 0 : PTE_NX);
        UINT64 first = vstart & ~0xFFFULL, last = (vend + 0xFFF) & ~0xFFFULL;

        for (UINT64 va = first; va < last; va += PAGE_SIZE) {
            UINT64 page = pmm_alloc_page();
            if (!page) return -1;
            memset((void*)(UINTN)page, 0, PAGE_SIZE);
            if (paging_map_in(cr3, va, page, flags) != 0) { pmm_free_page(page); return -1; }

            // Bu sayfaya dusen dosya baytlarini kopyala (sayfa fiziksel olarak birebir eslendiginden dogrudan yazilir)
            UINT64 fs = ph->p_vaddr, fe = ph->p_vaddr + ph->p_filesz;       // dosyadan gelen sanal aralik
            UINT64 cs = va > fs ? va : fs;
            UINT64 ce = (va + PAGE_SIZE) < fe ? (va + PAGE_SIZE) : fe;
            if (ce > cs)
                memcpy((void*)(UINTN)(page + (cs - va)), img + ph->p_offset + (cs - ph->p_vaddr), (size_t)(ce - cs));
        }
        loaded++;
    }
    if (!loaded) return -1;
    *entry = eh->e_entry;
    return 0;
}

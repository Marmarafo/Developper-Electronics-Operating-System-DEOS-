// DEOS -- bootloader ve kernel arasindaki ortak "sozlesme".
// Bootloader bunu doldurur, kernel'e pointer olarak gecirir.
#ifndef DEOS_BOOTINFO_H
#define DEOS_BOOTINFO_H
#include "types.h"

typedef struct {
    UINT64 framebuffer_base;
    UINT64 framebuffer_size;
    UINT32 width;
    UINT32 height;
    UINT32 pixels_per_scanline;

    UINT8  *memory_map;
    UINT64 memory_map_size;
    UINT64 memory_map_desc_size;
    UINT32 memory_map_desc_version;
} BootInfo;

#endif

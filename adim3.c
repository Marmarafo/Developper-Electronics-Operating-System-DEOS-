// DEOS UEFI on-yukleyici -- KERNEL.BIN'i diskten yukle, zipla.
// Adim 16: (a) GOP modu yalnizca 32bpp BGRX kabul edilir, degilse uygun mod aranir;
// (b) bellek haritasi icin dinamik tampon (BUFFER_TOO_SMALL buyumesi);
// (c) ACPI RSDP, UEFI ConfigurationTable'dan alinip BootInfo'ya tasınır.
#include "efi.h"
#include "bootinfo.h"

#define KERNEL_LOAD_ADDR 0x400000ULL

static EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *g_out;
static void put(const CHAR16 *s) { g_out->OutputString(g_out, (CHAR16*)s); }

static void put_hex(UINT64 n) {
    CHAR16 buf[20]; int i = 0;
    buf[i++] = L'0'; buf[i++] = L'x';
    if (n == 0) { buf[i++] = L'0'; buf[i] = 0; put(buf); return; }
    CHAR16 tmp[16]; int t = 0;
    while (n > 0 && t < 16) {
        int d = (int)(n & 0xF);
        tmp[t++] = (CHAR16)(d < 10 ? (L'0' + d) : (L'A' + d - 10));
        n >>= 4;
    }
    while (t > 0) buf[i++] = tmp[--t];
    buf[i] = 0;
    put(buf);
}

static void put_dec(UINTN n) {
    CHAR16 tmp[24], buf[24];
    int t = 0, i = 0;
    if (n == 0) { put(L"0"); return; }
    while (n > 0 && t < 23) { tmp[t++] = (CHAR16)(L'0' + (n % 10)); n /= 10; }
    while (t > 0) buf[i++] = tmp[--t];
    buf[i] = 0;
    put(buf);
}

static void put_status(EFI_STATUS st) {
    put(L"  [status="); put_hex((UINT64)st); put(L" -> ");
    UINT64 code = (UINT64)st & 0x7FFFFFFFFFFFFFFFULL;
    BOOLEAN is_err = EFI_ERROR(st);
    if (!is_err && code == 0) { put(L"EFI_SUCCESS"); }
    else if (is_err && code == 1)  put(L"EFI_LOAD_ERROR");
    else if (is_err && code == 2)  put(L"EFI_INVALID_PARAMETER");
    else if (is_err && code == 3)  put(L"EFI_UNSUPPORTED");
    else if (is_err && code == 4)  put(L"EFI_BAD_BUFFER_SIZE");
    else if (is_err && code == 5)  put(L"EFI_BUFFER_TOO_SMALL");
    else if (is_err && code == 6)  put(L"EFI_NOT_READY");
    else if (is_err && code == 7)  put(L"EFI_DEVICE_ERROR");
    else if (is_err && code == 8)  put(L"EFI_WRITE_PROTECTED");
    else if (is_err && code == 9)  put(L"EFI_OUT_OF_RESOURCES");
    else if (is_err && code == 10) put(L"EFI_VOLUME_CORRUPTED");
    else if (is_err && code == 12) put(L"EFI_NO_MEDIA");
    else if (is_err && code == 14) put(L"EFI_NOT_FOUND");
    else if (is_err && code == 15) put(L"EFI_ACCESS_DENIED");
    else if (is_err && code == 21) put(L"EFI_ABORTED");
    else put(L"BILINMEYEN/diger");
    put(L"]\r\n");
}

static void die(const CHAR16 *msg) { put(msg); while (1) { __asm__ __volatile__ ("hlt"); } }

static void busy_wait(unsigned int iterations) {
    for (volatile unsigned int i = 0; i < iterations; i++) { }
}

static UINT8 *g_mmap_buf = NULL;
static BootInfo g_bootinfo;

// UEFI ConfigurationTable kaydi: VendorGuid (16 byte) + VendorTable.
typedef struct { EFI_GUID VendorGuid; void *VendorTable; } EFI_CONF_TABLE;

static int guid_eq(const EFI_GUID *a, const EFI_GUID *b) {
    const UINT8 *x = (const UINT8*)a, *y = (const UINT8*)b;
    for (int i = 0; i < 16; i++) if (x[i] != y[i]) return 0;
    return 1;
}

static UINT64 find_rsdp(EFI_SYSTEM_TABLE *st) {
    // ACPI 2.0+ (XSDT) ve ACPI 1.0 (RSDT) GUID'leri.
    static const UINT8 g_acpi20[16] = { 0x71,0xE8,0x68,0x88,0xF1,0xE4,0xD3,0x11,0xBC,0x22,0x00,0x80,0xC7,0x3C,0x88,0x81 };
    static const UINT8 g_acpi10[16] = { 0x30,0x2D,0x9D,0xEB,0x88,0x2D,0xD3,0x11,0x9A,0x16,0x00,0x90,0x27,0x3F,0xC1,0x4D };
    EFI_CONF_TABLE *tab = (EFI_CONF_TABLE*)st->ConfigurationTable;
    for (UINTN i = 0; i < st->NumberOfTableEntries; i++) {
        if (guid_eq(&tab[i].VendorGuid, (const EFI_GUID*)g_acpi20) ||
            guid_eq(&tab[i].VendorGuid, (const EFI_GUID*)g_acpi10))
            return (UINT64)(UINTN)tab[i].VendorTable;
    }
    return 0;
}

// GOP'tan 32 bpp BGRX (PixelBlueGreenRedReserved8BitPerColor) mod bul; yoksa hata.
static EFI_GRAPHICS_OUTPUT_PROTOCOL *gop_bgrx(EFI_BOOT_SERVICES *bs) {
    EFI_GUID gop_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;
    if (EFI_ERROR(bs->LocateProtocol(&gop_guid, NULL, (void**)&gop)) || !gop)
        die(L"HATA: GOP bulunamadi.\r\n");

    UINTN info_size = 0;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info = NULL;
    UINT32 mode_count = gop->Mode->MaxMode;

    // Once aktif modu denetle.
    for (UINT32 pass = 0; pass < 2; pass++) {
        if (gop->Mode->Info->PixelFormat == PixelBlueGreenRedReserved8BitPerColor)
            return gop;
        for (UINT32 i = 0; i < mode_count; i++) {
            if (EFI_ERROR(gop->QueryMode(gop, i, &info_size, &info)) || !info) continue;
            if (info->PixelFormat == PixelBlueGreenRedReserved8BitPerColor &&
                info->PixelInformation.ReservedMask == 0xFF000000u) {
                if (!EFI_ERROR(gop->SetMode(gop, i))) {
                    put(L"GOP mod degisti -> BGRX 32bpp, index "); put_dec(i); put(L"\r\n");
                    return gop;
                }
            }
        }
        if (pass == 0) put(L"UYARI: aktif GOP modu 32bpp BGRX degil, mod araniyor...\r\n");
    }
    die(L"HATA: 32bpp BGRX GOP modu bulunamadi (kernel 0x00RRGGBB yazar).\r\n");
    return 0;
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    g_out = SystemTable->ConOut;
    EFI_BOOT_SERVICES *bs = SystemTable->BootServices;

    g_out->ClearScreen(g_out);
    put(L"DEOS UEFI on-yukleyici -- Adim 16\r\n");
    put(L"================================\r\n\r\n");

    // --- 1) GOP: BGRX 32bpp mod sec ---
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = gop_bgrx(bs);
    EFI_PHYSICAL_ADDRESS fb = gop->Mode->FrameBufferBase;
    UINT32 width  = gop->Mode->Info->HorizontalResolution;
    UINT32 height = gop->Mode->Info->VerticalResolution;
    UINT32 pitch  = gop->Mode->Info->PixelsPerScanLine;
    put(L"Framebuffer: "); put_hex((UINT64)fb);
    put(L"  Cozunurluk: "); put_dec(width); put(L"x"); put_dec(height);
    put(L"  Format: BGRX32\r\n");

    // --- 2) ACPI RSDP (ExitBootServices'ten once alinmali) ---
    UINT64 rsdp = find_rsdp(SystemTable);
    put(L"ACPI RSDP: "); put_hex(rsdp); put(L"\r\n");

    // --- 3) KERNEL.BIN'i disktan oku ---
    EFI_GUID li_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_LOADED_IMAGE_PROTOCOL *loaded_image = NULL;
    EFI_STATUS st = bs->HandleProtocol(ImageHandle, &li_guid, (void**)&loaded_image);
    if (EFI_ERROR(st)) { put(L"HATA: LoadedImageProtocol alinamadi."); put_status(st); die(L"\r\n"); }

    EFI_GUID sfs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *sfs = NULL;
    st = bs->HandleProtocol(loaded_image->DeviceHandle, &sfs_guid, (void**)&sfs);
    if (EFI_ERROR(st)) { put(L"HATA: SimpleFileSystem alinamadi."); put_status(st); die(L"\r\n"); }

    EFI_FILE_PROTOCOL *root = NULL;
    st = sfs->OpenVolume(sfs, &root);
    if (EFI_ERROR(st)) { put(L"HATA: OpenVolume basarisiz."); put_status(st); die(L"\r\n"); }

    EFI_FILE_PROTOCOL *kfile = NULL;
    st = root->Open(root, &kfile, L"\\EFI\\BOOT\\KERNEL.BIN", EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(st)) { put(L"HATA: KERNEL.BIN acilamadi."); put_status(st); die(L"\r\n"); }

    UINT64 kernel_size = 0;
    kfile->SetPosition(kfile, 0xFFFFFFFFFFFFFFFFULL);
    kfile->GetPosition(kfile, &kernel_size);
    kfile->SetPosition(kfile, 0);
    put(L"KERNEL.BIN boyutu: "); put_dec((UINTN)kernel_size); put(L" byte\r\n");

    UINTN pages = (UINTN)((kernel_size + 4095) / 4096);
    EFI_PHYSICAL_ADDRESS kernel_addr = KERNEL_LOAD_ADDR;
    st = bs->AllocatePages(AllocateAddress, EfiLoaderData, pages, &kernel_addr);
    if (EFI_ERROR(st)) { put(L"HATA: AllocatePages basarisiz."); put_status(st); die(L"\r\n"); }

    UINTN read_size = (UINTN)kernel_size;
    st = kfile->Read(kfile, &read_size, (void*)(UINTN)kernel_addr);
    if (EFI_ERROR(st)) { put(L"HATA: Kernel okunamadi."); put_status(st); die(L"\r\n"); }
    kfile->Close(kfile);
    put(L"Kernel yuklendi: "); put_hex((UINT64)kernel_addr); put(L"\r\n\r\n");

    // --- 4) Bellek haritasi: dinamik tampon + BUFFER_TOO_SMALL buyumesi ---
    UINTN mmap_size, map_key, desc_size;
    UINT32 desc_version;
    UINTN mmap_cap = 64 * 1024;
    for (;;) {
        EFI_PHYSICAL_ADDRESS maddr = 0;
        st = bs->AllocatePages(AllocateAnyPages, EfiLoaderData, mmap_cap / 4096, &maddr);
        if (EFI_ERROR(st)) { put(L"HATA: mmap tamponu ayrilamadi."); put_status(st); die(L"\r\n"); }
        g_mmap_buf = (UINT8*)(UINTN)maddr;
        mmap_size = mmap_cap;
        st = bs->GetMemoryMap(&mmap_size, g_mmap_buf, &map_key, &desc_size, &desc_version);
        if (!EFI_ERROR(st)) break;
        if ((UINT64)st == (UINT64)(0x8000000000000000ULL | 5)) { // EFI_BUFFER_TOO_SMALL
            UINTN need = mmap_size + 4096;
            ((EFI_STATUS (EFIAPI *)(EFI_PHYSICAL_ADDRESS, UINTN))bs->FreePages)(maddr, mmap_cap / 4096);
            mmap_cap = need;
            continue;
        }
        put(L"HATA: GetMemoryMap basarisiz."); put_status(st); die(L"\r\n");
    }

    // BootInfo bellek haritasi alanlarini ExitBootServices'ten ONCE doldur.
    // Ilk deneme basarili olursa bu degerler gecerli olur; hepsi yerel degiskenlerden
    // okunur, UEFI cagrisi yapilmaz.
    g_bootinfo.memory_map = g_mmap_buf;
    g_bootinfo.memory_map_size = mmap_size;
    g_bootinfo.memory_map_desc_size = desc_size;
    g_bootinfo.memory_map_desc_version = desc_version;

    put(L"ExitBootServices cagriliyor -- bundan sonra ConOut CALISMAYACAK...\r\n");
    busy_wait(30000000);

    // --- 5) ExitBootServices -- gerekirse mapKey ile yeniden dene ---
    EFI_STATUS exit_status = bs->ExitBootServices(ImageHandle, map_key);
    int retries = 0;
    while (EFI_ERROR(exit_status) && retries < 5) {
        mmap_size = mmap_cap;
        if (EFI_ERROR(bs->GetMemoryMap(&mmap_size, g_mmap_buf, &map_key,
                                       &desc_size, &desc_version))) break;
        exit_status = bs->ExitBootServices(ImageHandle, map_key);
        retries++;
    }
    if (EFI_ERROR(exit_status)) die(L"HATA: ExitBootServices basarisiz oldu.\r\n");

    // Yeniden deneme turunda GetMemoryMap yeniden calistiysa BootInfo'yu son degerlerle
    // guncelle (yine yalnizca yerel degiskenler; UEFI Boot Services artik kullanilmaz).
    g_bootinfo.memory_map = g_mmap_buf;
    g_bootinfo.memory_map_size = mmap_size;
    g_bootinfo.memory_map_desc_size = desc_size;
    g_bootinfo.memory_map_desc_version = desc_version;

    // BURADAN SONRA UEFI YOK.
    g_bootinfo.framebuffer_base = (UINT64)fb;
    g_bootinfo.framebuffer_size = (UINT64)pitch * height * 4;
    g_bootinfo.width = width;
    g_bootinfo.height = height;
    g_bootinfo.pixels_per_scanline = pitch;
    g_bootinfo.pixel_format = PixelBlueGreenRedReserved8BitPerColor;
    g_bootinfo.rsdp = rsdp;

    __asm__ __volatile__ ("cli");
    __asm__ __volatile__ ("outb %0, %1" : : "a"((UINT8)'J'), "Nd"((UINT16)0x3F8));

    void (*kernel_entry)(BootInfo*) = (void (*)(BootInfo*))(UINTN)kernel_addr;
    kernel_entry(&g_bootinfo);

    while (1) { __asm__ __volatile__ ("hlt"); }
}

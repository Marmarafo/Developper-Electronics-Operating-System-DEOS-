// DEOS UEFI on-yukleyici -- Adim 3: KERNEL.BIN'i diskten yukle, zipla.
// Adim 2'nin devami: artik ekrana animasyon cizmek yerine, ayri derlenmis
// bagimsiz bir KERNEL.BIN dosyasini \EFI\BOOT\KERNEL.BIN yolundan okuyup
// sabit bir fiziksel adrese (0x400000) yukluyoruz, sonra ExitBootServices
// sonrasi oraya CPU'yu zipliyoruz. Framebuffer bilgisi ve bellek haritasi
// BootInfo struct'i icinde kernel'e parametre olarak geciyor.
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

// EFI_STATUS degerini hem hex hem de bilinen hata isimleriyle yazdirir --
// artik "basarisiz oldu" deyip gecmiyoruz, TAM OLARAK HANGI HATA oldugunu
// goruyoruz. Boylece bir daha kor kor GUID tahmini yapmiyoruz.
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

static void busy_wait(unsigned int iterations) {
    for (volatile unsigned int i = 0; i < iterations; i++) { }
}

static UINT8 g_mmap_buffer[16384];
static BootInfo g_bootinfo;

EFI_STATUS EFIAPI efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    g_out = SystemTable->ConOut;
    EFI_BOOT_SERVICES *bs = SystemTable->BootServices;

    g_out->ClearScreen(g_out);
    put(L"DEOS UEFI on-yukleyici -- Adim 3: KERNEL.BIN yukleme\r\n");
    put(L"======================================================\r\n\r\n");

    // --- 1) GOP: mevcut aktif modu kullan (mod secimi sormuyoruz) ---
    EFI_GUID gop_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;
    if (EFI_ERROR(bs->LocateProtocol(&gop_guid, NULL, (void**)&gop)) || !gop) {
        put(L"HATA: GOP bulunamadi.\r\n");
        while (1) { __asm__ __volatile__ ("hlt"); }
    }
    EFI_PHYSICAL_ADDRESS fb = gop->Mode->FrameBufferBase;
    UINT32 width  = gop->Mode->Info->HorizontalResolution;
    UINT32 height = gop->Mode->Info->VerticalResolution;
    UINT32 pitch  = gop->Mode->Info->PixelsPerScanLine;
    put(L"Framebuffer: "); put_hex((UINT64)fb);
    put(L"  Cozunurluk: "); put_dec(width); put(L"x"); put_dec(height); put(L"\r\n");

    // --- 2) KERNEL.BIN'i disktan oku ---
    EFI_GUID li_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_LOADED_IMAGE_PROTOCOL *loaded_image = NULL;
    EFI_STATUS st_li = bs->HandleProtocol(ImageHandle, &li_guid, (void**)&loaded_image);
    if (EFI_ERROR(st_li)) {
        put(L"HATA: LoadedImageProtocol alinamadi."); put_status(st_li);
        while (1) { __asm__ __volatile__ ("hlt"); }
    }

    EFI_GUID sfs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *sfs = NULL;
    EFI_STATUS st_sfs = bs->HandleProtocol(loaded_image->DeviceHandle, &sfs_guid, (void**)&sfs);
    if (EFI_ERROR(st_sfs)) {
        put(L"HATA: SimpleFileSystem alinamadi."); put_status(st_sfs);
        while (1) { __asm__ __volatile__ ("hlt"); }
    }

    EFI_FILE_PROTOCOL *root = NULL;
    EFI_STATUS st_vol = sfs->OpenVolume(sfs, &root);
    if (EFI_ERROR(st_vol)) {
        put(L"HATA: OpenVolume basarisiz."); put_status(st_vol);
        while (1) { __asm__ __volatile__ ("hlt"); }
    }

    EFI_FILE_PROTOCOL *kfile = NULL;
    EFI_STATUS st_open = root->Open(root, &kfile, L"\\EFI\\BOOT\\KERNEL.BIN", EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(st_open)) {
        put(L"HATA: KERNEL.BIN acilamadi (yol: \\EFI\\BOOT\\KERNEL.BIN)."); put_status(st_open);
        while (1) { __asm__ __volatile__ ("hlt"); }
    }

    UINT64 kernel_size = 0;
    EFI_STATUS st_seek_end = kfile->SetPosition(kfile, 0xFFFFFFFFFFFFFFFFULL);
    if (EFI_ERROR(st_seek_end)) {
        put(L"HATA: SetPosition(EOF) basarisiz."); put_status(st_seek_end);
        while (1) { __asm__ __volatile__ ("hlt"); }
    }
    EFI_STATUS st_getpos = kfile->GetPosition(kfile, &kernel_size);
    if (EFI_ERROR(st_getpos)) {
        put(L"HATA: GetPosition basarisiz."); put_status(st_getpos);
        while (1) { __asm__ __volatile__ ("hlt"); }
    }
    EFI_STATUS st_seek_start = kfile->SetPosition(kfile, 0);
    if (EFI_ERROR(st_seek_start)) {
        put(L"HATA: SetPosition(0) basarisiz (basa sarilamadi)."); put_status(st_seek_start);
        while (1) { __asm__ __volatile__ ("hlt"); }
    }
    put(L"KERNEL.BIN boyutu: "); put_dec((UINTN)kernel_size); put(L" byte\r\n");

    UINTN pages = (UINTN)((kernel_size + 4095) / 4096);
    EFI_PHYSICAL_ADDRESS kernel_addr = KERNEL_LOAD_ADDR;
    EFI_STATUS st_alloc = bs->AllocatePages(AllocateAddress, EfiLoaderData, pages, &kernel_addr);
    if (EFI_ERROR(st_alloc)) {
        put(L"HATA: AllocatePages basarisiz (0x400000 dolu olabilir)."); put_status(st_alloc);
        while (1) { __asm__ __volatile__ ("hlt"); }
    }

    UINTN read_size = (UINTN)kernel_size;
    EFI_STATUS st_read = kfile->Read(kfile, &read_size, (void*)(UINTN)kernel_addr);
    if (EFI_ERROR(st_read)) {
        put(L"HATA: Kernel okunamadi."); put_status(st_read);
        while (1) { __asm__ __volatile__ ("hlt"); }
    }
    kfile->Close(kfile);
    put(L"Kernel yuklendi: "); put_hex((UINT64)kernel_addr); put(L"\r\n\r\n");

    // --- 3) Bellek haritasini al (retry mantigiyla) ---
    UINTN mmap_size, map_key, desc_size;
    UINT32 desc_version;
    EFI_STATUS mmap_status;
    int attempts = 0;
    do {
        mmap_size = sizeof(g_mmap_buffer);
        mmap_status = bs->GetMemoryMap(&mmap_size, g_mmap_buffer, &map_key, &desc_size, &desc_version);
        attempts++;
    } while (EFI_ERROR(mmap_status) && attempts < 3);

    if (EFI_ERROR(mmap_status)) {
        put(L"HATA: GetMemoryMap basarisiz.\r\n");
        while (1) { __asm__ __volatile__ ("hlt"); }
    }

    put(L"ExitBootServices cagriliyor -- bundan sonra ConOut CALISMAYACAK...\r\n");
    busy_wait(30000000);

    // --- 4) ExitBootServices -- gerekirse MapKey degisikligi icin yeniden dene ---
    EFI_STATUS exit_status = bs->ExitBootServices(ImageHandle, map_key);
    int retries = 0;
    while (EFI_ERROR(exit_status) && retries < 3) {
        mmap_size = sizeof(g_mmap_buffer);
        if (EFI_ERROR(bs->GetMemoryMap(&mmap_size, g_mmap_buffer, &map_key, &desc_size, &desc_version))) break;
        exit_status = bs->ExitBootServices(ImageHandle, map_key);
        retries++;
    }
    if (EFI_ERROR(exit_status)) {
        put(L"HATA: ExitBootServices basarisiz oldu.\r\n");
        while (1) { __asm__ __volatile__ ("hlt"); }
    }

    // ===================================================================
    // BURADAN SONRASI: ARTIK UEFI YOK. put()/ConOut/LocateProtocol
    // HICBIRI KULLANILAMAZ. Sadece kendi kodumuz ve dogrudan bellek erisimi.
    // ===================================================================

    g_bootinfo.framebuffer_base = (UINT64)fb;
    g_bootinfo.framebuffer_size = (UINT64)pitch * height * 4;
    g_bootinfo.width = width;
    g_bootinfo.height = height;
    g_bootinfo.pixels_per_scanline = pitch;
    g_bootinfo.memory_map = g_mmap_buffer;
    g_bootinfo.memory_map_size = mmap_size;
    g_bootinfo.memory_map_desc_size = desc_size;
    g_bootinfo.memory_map_desc_version = desc_version;

    __asm__ __volatile__ ("cli");
    // Hata ayiklama: seri porta 'J' (jump) -- kernel'e ziplamak uzereyiz.
    __asm__ __volatile__ ("outb %0, %1" : : "a"((UINT8)'J'), "Nd"((UINT16)0x3F8));

    void (*kernel_entry)(BootInfo*) = (void (*)(BootInfo*))(UINTN)kernel_addr;
    kernel_entry(&g_bootinfo);

    // Kernel normalde hic donmemeli; donerse burada takilalim.
    while (1) { __asm__ __volatile__ ("hlt"); }
}

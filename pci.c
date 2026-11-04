// DEOS -- PCI veriyolu taramasi (klasik config mekanizmasi #1: 0xCF8 adres, 0xCFC veri).
#include "types.h"
#include "pci.h"
#include "io.h"

#define MAX_PCI 64
static pci_dev_t g_devs[MAX_PCI];
static int g_n;

static UINT32 cfg_read(UINT8 bus, UINT8 dev, UINT8 fn, UINT8 off) {
    outl(0xCF8, 0x80000000u | ((UINT32)bus << 16) | ((UINT32)dev << 11) | ((UINT32)fn << 8) | (off & 0xFC));
    return inl(0xCFC);
}

static void cfg_write(UINT8 bus, UINT8 dev, UINT8 fn, UINT8 off, UINT32 v) {
    outl(0xCF8, 0x80000000u | ((UINT32)bus << 16) | ((UINT32)dev << 11) | ((UINT32)fn << 8) | (off & 0xFC));
    outl(0xCFC, v);
}

static void add(UINT8 bus, UINT8 dev, UINT8 fn) {
    UINT32 id = cfg_read(bus, dev, fn, 0);
    if ((id & 0xFFFF) == 0xFFFF || g_n >= MAX_PCI) return;
    UINT32 cls = cfg_read(bus, dev, fn, 8);
    UINT32 hdr = cfg_read(bus, dev, fn, 0x0C);
    pci_dev_t *d = &g_devs[g_n++];
    d->bus = bus; d->dev = dev; d->fn = fn;
    d->vendor = (UINT16)id; d->device = (UINT16)(id >> 16);
    d->class_code = (UINT8)(cls >> 24); d->subclass = (UINT8)(cls >> 16); d->prog_if = (UINT8)(cls >> 8);
    d->header_type = (UINT8)(hdr >> 16);
}

void pci_scan(void) {
    g_n = 0;
    for (int bus = 0; bus < 256; bus++)
        for (int dev = 0; dev < 32; dev++) {
            if ((cfg_read((UINT8)bus, (UINT8)dev, 0, 0) & 0xFFFF) == 0xFFFF) continue;
            add((UINT8)bus, (UINT8)dev, 0);
            if (cfg_read((UINT8)bus, (UINT8)dev, 0, 0x0C) & 0x800000)       // cok islevli aygit
                for (int fn = 1; fn < 8; fn++) add((UINT8)bus, (UINT8)dev, (UINT8)fn);
        }
}

int pci_count(void) { return g_n; }
const pci_dev_t *pci_get(int i) { return (i >= 0 && i < g_n) ? &g_devs[i] : 0; }

const pci_dev_t *pci_find_class(UINT8 cls, UINT8 sub, int prog_if) {
    for (int i = 0; i < g_n; i++)
        if (g_devs[i].class_code == cls && g_devs[i].subclass == sub && (prog_if < 0 || g_devs[i].prog_if == prog_if))
            return &g_devs[i];
    return 0;
}

UINT32 pci_read32(const pci_dev_t *d, UINT8 off) { return cfg_read(d->bus, d->dev, d->fn, off); }
void   pci_write32(const pci_dev_t *d, UINT8 off, UINT32 v) { cfg_write(d->bus, d->dev, d->fn, off, v); }

void pci_enable(const pci_dev_t *d) {
    UINT32 cmd = pci_read32(d, 4);
    pci_write32(d, 4, (cmd & 0xFFFF) | 0x06);      // bit1: bellek, bit2: bus master
}

UINT64 pci_bar(const pci_dev_t *d, int n) {
    UINT32 lo = pci_read32(d, (UINT8)(0x10 + n * 4));
    if (lo & 1) return 0;                           // I/O BAR
    UINT64 base = lo & 0xFFFFFFF0u;
    if (((lo >> 1) & 3) == 2 && n < 5) base |= (UINT64)pci_read32(d, (UINT8)(0x14 + n * 4)) << 32;   // 64 bit BAR
    return base;
}

const char *pci_class_name(UINT8 c, UINT8 s) {
    switch ((c << 8) | s) {
    case 0x0000: return "Eski aygit";           case 0x0100: return "SCSI denetleyici";
    case 0x0101: return "IDE denetleyici";      case 0x0106: return "SATA (AHCI)";
    case 0x0108: return "NVMe";                 case 0x0200: return "Ethernet";
    case 0x0300: return "VGA ekran";            case 0x0380: return "Ekran (diger)";
    case 0x0401: return "Ses (coklu ortam)";    case 0x0403: return "Ses (HD Audio)";
    case 0x0600: return "Ana kopru (host)";     case 0x0601: return "ISA kopru";
    case 0x0604: return "PCI-PCI kopru";        case 0x0680: return "Kopru (diger)";
    case 0x0C03: return "USB denetleyici";      case 0x0C05: return "SMBus";
    default:     return "Diger";
    }
}

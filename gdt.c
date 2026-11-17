// DEOS -- Global Descriptor Table (GDT) + Task State Segment (TSS).
//
//   0x00 null
//   0x08 kernel kod (ring 0)      0x10 kernel veri (ring 0)
//   0x18 kullanici veri (ring 3)  0x20 kullanici kod (ring 3)   <- ileride (syscall/sysret duzeni)
//   0x28 TSS (16 bayt, iki girdi)
//
// TSS'in IST (Interrupt Stack Table) alani sayesinde bazi kesmeler HER ZAMAN
// kendi ayri stack'inde calisir: #DF (double fault), NMI, #MC. Boylece kernel
// stack'i tassa bile (ornegin koruma sayfasina dusup) panik ekrani yine gorunur.
#include "types.h"
#include "gdt.h"

typedef struct __attribute__((packed)) {
    UINT16 limit;
    UINT64 base;
} gdt_ptr_t;

typedef struct __attribute__((packed)) {
    UINT32 reserved0;
    UINT64 rsp[3];          // ring 0/1/2 stack'leri (ring 3 -> 0 gecislerinde kullanilir)
    UINT64 reserved1;
    UINT64 ist[7];          // IST1..IST7
    UINT64 reserved2;
    UINT16 reserved3;
    UINT16 iopb_offset;
} tss_t;

#define IST_STACK_SIZE 16384

static UINT64   g_gdt[7] __attribute__((aligned(16)));
static gdt_ptr_t g_gdt_ptr;
static tss_t    g_tss __attribute__((aligned(16)));
static UINT8    g_ist_stacks[3][IST_STACK_SIZE] __attribute__((aligned(16)));

extern void gdt_flush(UINT64 gdt_ptr_addr);   // gdt_flush.S
extern char stack_top[];                       // entry.S

void gdt_init(void) {
    g_gdt[0] = 0x0000000000000000ULL;   // null
    g_gdt[1] = 0x00AF9A000000FFFFULL;   // kernel code
    g_gdt[2] = 0x00CF92000000FFFFULL;   // kernel data
    g_gdt[3] = 0x00CFF2000000FFFFULL;   // user data (DPL 3)
    g_gdt[4] = 0x00AFFA000000FFFFULL;   // user code (DPL 3, 64-bit)

    for (UINT32 i = 0; i < sizeof(g_tss); i++) ((UINT8*)&g_tss)[i] = 0;
    g_tss.rsp[0]      = (UINT64)(UINTN)stack_top;
    g_tss.ist[0]      = (UINT64)(UINTN)&g_ist_stacks[0][IST_STACK_SIZE];  // IST1: #DF
    g_tss.ist[1]      = (UINT64)(UINTN)&g_ist_stacks[1][IST_STACK_SIZE];  // IST2: NMI
    g_tss.ist[2]      = (UINT64)(UINTN)&g_ist_stacks[2][IST_STACK_SIZE];  // IST3: #MC
    g_tss.iopb_offset = sizeof(g_tss);                                    // G/C izin bitmap'i yok

    UINT64 base  = (UINT64)(UINTN)&g_tss;
    UINT64 limit = sizeof(g_tss) - 1;
    g_gdt[5] = (limit & 0xFFFF) | ((base & 0xFFFFFF) << 16) | (0x89ULL << 40)   // present, 64-bit TSS (available)
             | (((limit >> 16) & 0xF) << 48) | (((base >> 24) & 0xFF) << 56);
    g_gdt[6] = base >> 32;

    g_gdt_ptr.limit = (UINT16)(sizeof(g_gdt) - 1);
    g_gdt_ptr.base  = (UINT64)(UINTN)&g_gdt;
    gdt_flush((UINT64)(UINTN)&g_gdt_ptr);

    __asm__ __volatile__ ("ltr %w0" : : "r"((UINT16)0x28));
}

void tss_set_rsp0(unsigned long long rsp0) { g_tss.rsp[0] = rsp0; }

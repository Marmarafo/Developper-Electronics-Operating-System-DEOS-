#include "progs.h"
extern const unsigned char _binary_hello_elf_start[], _binary_hello_elf_end[];
extern const unsigned char _binary_ticker_elf_start[], _binary_ticker_elf_end[];
extern const unsigned char _binary_heaptest_elf_start[], _binary_heaptest_elf_end[];
extern const unsigned char _binary_reverse_elf_start[], _binary_reverse_elf_end[];
extern const unsigned char _binary_badboy_elf_start[], _binary_badboy_elf_end[];
extern const unsigned char _binary_fault_elf_start[], _binary_fault_elf_end[];
const embedded_prog_t g_progs[] = {
    { "hello", _binary_hello_elf_start, _binary_hello_elf_end },
    { "ticker", _binary_ticker_elf_start, _binary_ticker_elf_end },
    { "heaptest", _binary_heaptest_elf_start, _binary_heaptest_elf_end },
    { "reverse", _binary_reverse_elf_start, _binary_reverse_elf_end },
    { "badboy", _binary_badboy_elf_start, _binary_badboy_elf_end },
    { "fault", _binary_fault_elf_start, _binary_fault_elf_end },
    { 0, 0, 0 }
};

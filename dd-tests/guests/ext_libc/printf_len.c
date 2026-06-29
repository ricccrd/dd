// printf length modifiers: hh h l ll z j t. Oracle (raw output).
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

int main(void) {
    printf("[%hhd][%hd][%ld][%lld]\n", (signed char)-1, (short)-2, -3L, -4LL);
    printf("[%hhu][%hu][%lu][%llu]\n", (unsigned char)250, (unsigned short)60000,
           123456789UL, 12345678901234ULL);
    printf("[%zu][%zd][%jd][%td]\n", (size_t)100, (ssize_t)-100,
           (intmax_t)-9, (ptrdiff_t)-5);
    printf("[%lx][%llx]\n", 0xdeadbeefUL, 0x123456789aULL);
    return 0;
}

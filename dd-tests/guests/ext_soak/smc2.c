// SOAK: self-modifying-code re-translation, multi-instruction variant. Where soak_smc patches a single
// `movz` immediate, this holds a 3-instruction aarch64 function (movz w0,#lo ; movk w0,#hi,lsl#16 ; ret)
// in an RWX page and, 200k times, rewrites BOTH immediate fields, flushes the icache, and calls it --
// so two instructions in the same block change every iteration, producing a fresh 32-bit result and
// forcing the DBT to re-translate the whole block each time. This stresses per-address invalidation and
// code-cache recycle even harder than the single-instruction case: any stale translation returns the old
// 32-bit value and the checksum diverges. aarch64 machine code only; diffed against a native run (oracle).
// xfail aarch64: matches the documented SMC gap; mmap(RWX) is also EPERM on darwin W^X (see GAPS.md).
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>

#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif

int main(void) {
    uint32_t *code = mmap(0, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (code == MAP_FAILED) { perror("mmap"); return 1; }
    code[2] = 0xd65f03c0; // ret
    uint64_t sum = 0;
    for (uint32_t i = 0; i < 200000; i++) {
        uint16_t lo = (uint16_t)(i & 0xffff);
        uint16_t hi = (uint16_t)((i * 2654435761u) & 0xffff);
        code[0] = 0x52800000u | ((uint32_t)lo << 5);  // movz w0, #lo
        code[1] = 0x72a00000u | ((uint32_t)hi << 5);  // movk w0, #hi, lsl #16
        __builtin___clear_cache((char *)code, (char *)code + 12);
        uint32_t (*f)(void) = (uint32_t (*)(void))code;
        sum += f(); // must observe ((hi<<16)|lo) from the just-written pair, never a stale translation
    }
    munmap(code, 4096);
    printf("soak smc2 sum=%llu\n", (unsigned long long)sum);
    return 0;
}

// SOAK: self-modifying-code re-translation (the hardest DBT endurance path). We hold a tiny aarch64
// function in an RWX page (`movz w0,#imm; ret`), and 200k times: patch its immediate, flush the icache
// (__builtin___clear_cache), and call it. Each patch produces a NEW code version at the SAME address,
// forcing the JIT to notice the change and re-translate -- unbounded distinct translations over the run,
// which churns the code cache (eviction/recycle) and the per-address translation invalidation. A DBT
// that ever serves a stale translation returns the wrong immediate and the checksum diverges. aarch64
// guest only (raw machine code); diffed against a native run -> oracle.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

int main(void) {
    uint32_t *code = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (code == MAP_FAILED) { perror("mmap"); return 1; }
    code[1] = 0xd65f03c0; // ret
    uint64_t sum = 0;
    for (uint32_t i = 0; i < 200000; i++) {
        uint16_t imm = (uint16_t)(i & 0xffff);
        code[0] = 0x52800000u | ((uint32_t)imm << 5); // movz w0, #imm
        __builtin___clear_cache((char *)code, (char *)code + 8); // signal the I-cache/DBT: code changed
        uint32_t (*f)(void) = (uint32_t (*)(void))code;
        sum += f(); // must observe the just-written immediate, never a stale translation
    }
    munmap(code, 4096);
    printf("soak smc sum=%llu\n", (unsigned long long)sum); // sum of (i & 0xffff), i=0..199999
    return 0;
}

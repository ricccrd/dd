// SOAK: bit-manipulation opcode endurance. A long (~80M iter) loop folds a running word through
// rotates, population-count, count-trailing-zeros, byte-swap and variable shifts -- the instruction
// family (RBIT/CLZ/RBIT+CLZ for ctz, REV, popcount-via-cnt on aarch64; BSWAP/POPCNT/TZCNT or their
// software fallbacks on x86) the JIT must lower correctly every single time. Over tens of millions of
// iterations any rare mis-lowering of one of these (wrong shift mask, off-by-one rotate, signed CLZ)
// diverges the checksum. Operands are kept nonzero where the builtins require it. Golden, every engine.
#include <stdint.h>
#include <stdio.h>

static inline uint64_t rotl(uint64_t v, unsigned r) { r &= 63; return (v << r) | (v >> ((64 - r) & 63)); }

int main(void) {
    uint64_t r = 0x0123456789abcdefULL, acc = 0;
    for (uint64_t i = 0; i < 80000000ULL; i++) {
        r = r * 6364136223846793005ULL + 1442695040888963407ULL; // LCG
        uint64_t v = r | 1ULL;                                    // ensure nonzero for ctz/clz
        acc += __builtin_popcountll(v);
        acc ^= rotl(v, (unsigned)(i & 63));
        acc += __builtin_ctzll(v);
        acc ^= (uint64_t)__builtin_clzll(v) << 8;
        acc += __builtin_bswap64(v);
        acc ^= (v >> (i & 31)) | (v << (i & 15));
    }
    printf("soak bitchurn acc=%llu\n", (unsigned long long)acc);
    return 0;
}

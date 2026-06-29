// SOAK: integer divide / modulo endurance. ~60M iterations of signed and unsigned 64-bit div and mod
// with continuously varying, data-dependent operands. Division is the one integer op that is NOT a
// single cheap instruction everywhere (aarch64 SDIV/UDIV + a separate MSUB for the remainder; on some
// lowerings a magic-number sequence or a helper call), so a long run of varied divisors is the way to
// catch a wrong remainder, a sign-handling slip, or a magic-constant mistake that a couple of short
// divisions would miss. Divisors are forced nonzero. Deterministic checksum -> golden, every engine.
#include <stdint.h>
#include <stdio.h>

int main(void) {
    uint64_t r = 0xcafef00dULL, uacc = 0;
    int64_t sacc = 0;
    for (uint64_t i = 0; i < 60000000ULL; i++) {
        r = r * 6364136223846793005ULL + 1442695040888963407ULL; // LCG
        uint64_t d = (r >> 17) | 1ULL;                            // nonzero unsigned divisor
        uacc += (r / d) ^ (r % d);
        int64_t sn = (int64_t)r;
        int64_t sd = (int64_t)((r >> 23) | 1ULL);                 // nonzero signed divisor
        if (sd == -1) sd = 3;                                     // avoid INT64_MIN/-1 overflow
        sacc += (sn / sd) - (sn % sd);
    }
    printf("soak divchurn uacc=%llu sacc=%lld\n", (unsigned long long)uacc, (long long)sacc);
    return 0;
}

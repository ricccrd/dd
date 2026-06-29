// Long-running compute: a few hundred million iterations of integer mixing. This keeps the
// JIT executing translated blocks for a sustained stretch (multi-second), shaking out block-
// cache eviction, counter overflow, and long-run drift. Deterministic -> oracle-checked.
#include <stdint.h>
#include <stdio.h>

int main(void) {
    uint64_t acc = 1469598103934665603ULL; // FNV offset
    for (uint64_t i = 0; i < 300000000ULL; i++) {
        acc ^= i;
        acc *= 1099511628211ULL; // FNV prime
        acc = (acc << 7) | (acc >> 57); // rotate so it can't settle
    }
    printf("busyloop acc=%llu\n", (unsigned long long)acc);
    return 0;
}

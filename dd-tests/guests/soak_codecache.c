// SOAK: block-chaining / dispatcher / IBTC endurance. A 256-arm switch (256 distinct translated
// blocks) is driven in a data-dependent, ever-changing order for ~120M iterations, so the engine's
// block lookup, chaining links, and indirect-branch target cache are exercised continuously over a
// long run -- the regime where a stale chain link or poisoned IBTC entry (fine for a short test)
// eventually returns a wrong block. Deterministic checksum -> golden, runs on every engine.
#include <stdint.h>
#include <stdio.h>

// 256 arms, each a distinct constant mix so every case is its own block with its own effect.
static uint64_t step(int k, uint64_t a) {
    switch (k & 0xff) {
#define ARM(n) case n: return (a ^ (0x9e3779b97f4a7c15ULL * (n + 1))) + ((a << ((n & 31) + 1)) | (a >> (63 - (n & 31))));
#define ARM8(b) ARM(b+0) ARM(b+1) ARM(b+2) ARM(b+3) ARM(b+4) ARM(b+5) ARM(b+6) ARM(b+7)
#define ARM64(b) ARM8(b+0) ARM8(b+8) ARM8(b+16) ARM8(b+24) ARM8(b+32) ARM8(b+40) ARM8(b+48) ARM8(b+56)
        ARM64(0) ARM64(64) ARM64(128) ARM64(192)
    }
    return a;
}

int main(void) {
    uint64_t a = 0x12345678ULL;
    for (uint64_t i = 0; i < 120000000ULL; i++)
        a = step((int)(a ^ (a >> 11)), a); // next arm depends on the running state -> unpredictable order
    printf("soak codecache acc=%llu\n", (unsigned long long)a);
    return 0;
}

// SOAK: code-cache eviction under MANY distinct hot blocks. Where soak_codecache drives 256 arms,
// this drives 1024 distinct translated blocks (a 1024-case switch) in a data-dependent order for
// ~60M iterations. With 4x the block population the JIT's code cache is far more likely to evict and
// re-translate live blocks mid-run, so a recycle/eviction bug (stale chain target, freed-then-reused
// slot, wrong-block dispatch) that never fires at 256 blocks surfaces here. Deterministic checksum
// -> golden, runs on every engine.
#include <stdint.h>
#include <stdio.h>

static uint64_t step(int k, uint64_t a) {
    switch (k & 0x3ff) {
#define ARM(n) case n: return (a ^ (0x9e3779b97f4a7c15ULL * (n + 1))) + ((a << ((n & 31) + 1)) | (a >> (63 - (n & 31))));
#define ARM8(b) ARM(b+0) ARM(b+1) ARM(b+2) ARM(b+3) ARM(b+4) ARM(b+5) ARM(b+6) ARM(b+7)
#define ARM64(b) ARM8(b+0) ARM8(b+8) ARM8(b+16) ARM8(b+24) ARM8(b+32) ARM8(b+40) ARM8(b+48) ARM8(b+56)
#define ARM256(b) ARM64(b+0) ARM64(b+64) ARM64(b+128) ARM64(b+192)
#define ARM1024(b) ARM256(b+0) ARM256(b+256) ARM256(b+512) ARM256(b+768)
        ARM1024(0)
    }
    return a;
}

int main(void) {
    uint64_t a = 0x12345678ULL;
    for (uint64_t i = 0; i < 60000000ULL; i++)
        a = step((int)(a ^ (a >> 13)), a); // next block depends on running state -> unpredictable order
    printf("soak manyblocks acc=%llu\n", (unsigned long long)a);
    return 0;
}

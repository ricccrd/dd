// SOAK: heap/brk/mmap endurance. ~6M malloc/free cycles of widely varied sizes (8B..256KB), touching
// each block so pages are actually committed. Drives sustained brk growth + large-alloc mmap/munmap
// churn through the runtime's memory syscalls over a long run -- where an off-by-one in size charging,
// an munmap leak, or brk drift slowly corrupts/exhausts the heap (invisible on a short test).
// Deterministic checksum of the touched bytes -> golden, runs on every engine.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    uint64_t sum = 0, r = 0x1234;
    void *live[64] = {0};
    size_t lsz[64] = {0};
    for (uint64_t i = 0; i < 6000000ULL; i++) {
        r = r * 6364136223846793005ULL + 1442695040888963407ULL; // LCG
        int slot = (int)((r >> 33) & 63);
        if (live[slot]) { // free the old occupant, folding a byte in so frees can't be elided
            sum += ((unsigned char *)live[slot])[0];
            free(live[slot]);
        }
        size_t sz = 8 + (size_t)((r >> 17) % (256 * 1024));
        unsigned char *p = malloc(sz);
        if (!p) { printf("soak allocchurn oom@%llu\n", (unsigned long long)i); return 1; }
        p[0] = (unsigned char)(r);
        p[sz - 1] = (unsigned char)(r >> 8); // touch both ends (commit first + last page)
        sum += p[sz - 1];
        live[slot] = p;
        lsz[slot] = sz;
    }
    for (int i = 0; i < 64; i++) if (live[i]) { sum += ((unsigned char *)live[i])[lsz[i] - 1]; free(live[i]); }
    printf("soak allocchurn sum=%llu\n", (unsigned long long)sum);
    return 0;
}

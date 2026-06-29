// SOAK: realloc grow/shrink endurance. ~2M realloc cycles on a single live block whose size swings
// wildly (8B .. 128KB) via an LCG, so the allocator constantly grows-in-place, shrinks, and relocates
// (copying contents). After each realloc the new tail byte is written and an invariant byte at offset 0
// is checked, so a realloc that loses or fails to preserve data is caught. This exercises the
// grow/shrink/relocate path of the runtime's heap (a different code path from plain malloc/free churn)
// over a long run, where a copy-on-grow slip or size-charging drift accumulates. Deterministic checksum.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    uint64_t r = 0x55aa55aaULL, sum = 0;
    size_t cur = 16;
    unsigned char *p = malloc(cur);
    if (!p) return 1;
    p[0] = 0xa5;                       // invariant marker preserved across reallocs
    for (uint64_t i = 0; i < 2000000ULL; i++) {
        r = r * 6364136223846793005ULL + 1442695040888963407ULL; // LCG
        size_t nsz = 8 + (size_t)((r >> 19) % (128 * 1024));
        unsigned char *q = realloc(p, nsz);
        if (!q) { printf("soak reallocchurn oom@%llu\n", (unsigned long long)i); free(p); return 1; }
        p = q;
        if (p[0] != 0xa5) { printf("soak reallocchurn lost-marker@%llu\n", (unsigned long long)i); return 1; }
        p[nsz - 1] = (unsigned char)r; // touch new tail
        sum += (uint64_t)p[nsz - 1] + nsz;
        cur = nsz;
    }
    sum += p[0];
    free(p);
    printf("soak reallocchurn sum=%llu\n", (unsigned long long)sum);
    return 0;
}

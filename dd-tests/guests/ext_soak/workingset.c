// SOAK: growing/shrinking working-set sweep. A single 32MB buffer is swept repeatedly with a window
// that oscillates from 64KB up to the full 32MB and back, cache-line stride, for many passes. The
// resident/active set therefore continuously grows and shrinks, churning the cache hierarchy and TLB
// coverage without any syscalls -- a pure-memory endurance stressor of the JIT's load/store lowering and
// address translation over a long run. The fold is order-fixed so the final checksum is deterministic.
// Golden, runs on every engine.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    size_t n = 32 * 1024 * 1024;
    unsigned char *buf = malloc(n);
    if (!buf) return 1;
    for (size_t i = 0; i < n; i++) buf[i] = (unsigned char)(i * 31 + 7); // init
    uint64_t sum = 0;
    // window grows x2 from 64KB to 32MB then shrinks back; sweep each window 4 times.
    for (int dir = 0; dir < 2; dir++) {
        for (size_t w = 64 * 1024; w <= n && w >= 64 * 1024; w = dir ? w / 2 : w * 2) {
            for (int pass = 0; pass < 4; pass++)
                for (size_t off = 0; off < w; off += 64) {       // cache-line stride
                    buf[off] = (unsigned char)(buf[off] + 1);
                    sum += buf[off];
                }
            if (dir == 0 && w == n) break;   // reached the top; let the shrink phase take over
            if (dir == 1 && w == 64 * 1024) break;
        }
    }
    free(buf);
    printf("soak workingset sum=%llu\n", (unsigned long long)sum);
    return 0;
}

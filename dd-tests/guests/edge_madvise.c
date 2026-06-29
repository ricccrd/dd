// EDGE: madvise(MADV_DONTNEED) on an anonymous mapping must DROP the pages — a subsequent read
// returns freshly-zeroed memory (Linux semantics; jemalloc/redis/glibc-arena rely on this). A
// runtime that no-ops madvise returns the STALE data instead. Diffed vs native -> oracle.
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

#define SZ (256 * 4096)

int main(void) {
    unsigned char *m = mmap(NULL, SZ, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m == MAP_FAILED) { perror("mmap"); return 1; }
    memset(m, 0xAB, SZ);
    long before = 0;
    for (int i = 0; i < SZ; i += 4096) before += m[i];
    int rc = madvise(m, SZ, MADV_DONTNEED);
    long after = 0;
    for (int i = 0; i < SZ; i += 4096) after += m[i]; // Linux: all zero after DONTNEED
    munmap(m, SZ);
    printf("madvise rc=%d before=%ld after=%ld\n", rc, before, after); // 0, 256*171, 0
    return 0;
}

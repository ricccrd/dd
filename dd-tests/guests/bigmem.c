// Large sparse allocation: mmap 512 MiB anonymous, touch one byte per page across the whole
// range (forcing demand-zero page faults), checksum the touched pages, then munmap. Stresses
// the guest page-fault path and large address-space handling. Deterministic -> oracle-checked.
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>

#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON // macOS spells it MAP_ANON
#endif

#define SZ (512UL << 20)
#define PG 4096UL

int main(void) {
    unsigned char *m = mmap(NULL, SZ, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m == MAP_FAILED) { perror("mmap"); return 1; }
    uint64_t sum = 0;
    for (uint64_t off = 0; off < SZ; off += PG) {
        m[off] = (unsigned char)((off / PG) & 0xff);
        sum += m[off];
    }
    munmap(m, SZ);
    printf("bigmem pages=%lu sum=%llu\n", SZ / PG, (unsigned long long)sum);
    return 0;
}

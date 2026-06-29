// SOAK: mmap/munmap syscall endurance. 400k times: mmap a fresh varied-size anonymous region
// (PROT_READ|WRITE, no EXEC -> portable under macOS W^X), write its first and last byte (committing the
// first and last page), fold a byte into a checksum, then munmap it. This drives ~800k mmap/munmap
// round-trips through the runtime's memory syscalls + page-fault path over a long run -- where an
// address-space-bookkeeping leak, a size-rounding off-by-one, or a region freed-but-not-unmapped slowly
// exhausts or corrupts the guest map (invisible to a handful of mmaps). Deterministic checksum -> golden.
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif

int main(void) {
    uint64_t r = 0xabcd1234ULL, sum = 0;
    long pg = sysconf(_SC_PAGESIZE);
    if (pg <= 0) pg = 4096;
    for (uint64_t i = 0; i < 400000ULL; i++) {
        r = r * 6364136223846793005ULL + 1442695040888963407ULL; // LCG
        size_t pages = 1 + (size_t)((r >> 20) & 7);              // 1..8 pages
        size_t len = pages * (size_t)pg;
        unsigned char *p = mmap(0, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) { printf("soak mmapchurn fail@%llu\n", (unsigned long long)i); return 1; }
        p[0] = (unsigned char)r;            // commit first page
        p[len - 1] = (unsigned char)(r >> 8); // commit last page
        sum += (uint64_t)p[0] + p[len - 1];
        munmap(p, len);
    }
    printf("soak mmapchurn sum=%llu\n", (unsigned long long)sum);
    return 0;
}

// SOAK: page-fault / TLB churn via repeatedly faulted large mappings. 3000 times: mmap a fresh 2MB
// anonymous region, then stride across it touching one byte in every 4KB page (512 first-touch faults),
// fold a checksum, and munmap it. That is ~1.5M demand page faults plus 6000 mmap/munmap over the run --
// a sustained hammering of the runtime's page-fault handler and TLB/shadow-map bookkeeping. A fault path
// that mismaps a page, mis-tracks a freshly faulted region, or leaks shadow entries drifts the checksum
// or fails the map only after the fault machinery has been pushed hard. Deterministic checksum -> golden.
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif

// Fixed 4KB stride (NOT sysconf page size) so the touch pattern + checksum are identical regardless of
// the host/guest page size — the JIT emulates 16KB pages while the build host uses 4KB, and a sysconf
// stride would make the golden value page-size-dependent. A 4KB stride still faults every real page.
#define STRIDE 4096

int main(void) {
    size_t len = 2 * 1024 * 1024;
    uint64_t sum = 0;
    for (uint64_t it = 0; it < 3000ULL; it++) {
        unsigned char *p = mmap(0, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) { printf("soak pagefault fail@%llu\n", (unsigned long long)it); return 1; }
        for (size_t off = 0; off < len; off += STRIDE) {
            p[off] = (unsigned char)(it + off);   // first-touch fault across the region
            sum += p[off];
        }
        munmap(p, len);
    }
    printf("soak pagefault sum=%llu\n", (unsigned long long)sum);
    return 0;
}

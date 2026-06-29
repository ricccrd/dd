// madvise on an anonymous mapping: NORMAL/WILLNEED/DONTNEED should all return 0 (advisory).
// After MADV_DONTNEED the pages must still be readable (re-faulted as zero for anon private).
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

int main(void) {
    long ps = sysconf(_SC_PAGESIZE);
    size_t len = ps * 4;
    char *m = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    int n = madvise(m, len, MADV_NORMAL) == 0;
    int wn = madvise(m, len, MADV_WILLNEED) == 0;
    memset(m, 7, len);
    int dn = madvise(m, len, MADV_DONTNEED) == 0;
    int readable = m[0] == 0 || m[0] == 7; // either re-zeroed (Linux) or retained (mac); just must not fault
    munmap(m, len);
    printf("madvise normal=%d willneed=%d dontneed=%d readable=%d\n", n, wn, dn, readable);
    return 0;
}

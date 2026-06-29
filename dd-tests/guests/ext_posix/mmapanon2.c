// MAP_ANONYMOUS read/write, then mprotect to PROT_READ (success rc), then munmap.
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

int main(void) {
    long ps = sysconf(_SC_PAGESIZE);
    size_t len = ps * 4;
    char *m = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    int mapped = m != MAP_FAILED;
    // anon memory starts zeroed
    int zeroed = m[0] == 0 && m[len - 1] == 0;
    memset(m, 0xAB, len);
    long sum = 0;
    for (size_t i = 0; i < len; i++) sum += (unsigned char)m[i];
    int wrote = sum == (long)len * 0xAB;
    int prot = mprotect(m, len, PROT_READ) == 0; // downgrade succeeds
    int unmap = munmap(m, len) == 0;
    printf("mmapanon2 mapped=%d zeroed=%d wrote=%d mprotect=%d munmap=%d\n", mapped, zeroed, wrote, prot, unmap);
    return 0;
}

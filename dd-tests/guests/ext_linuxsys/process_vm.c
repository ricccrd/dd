// process_vm_readv(2): read our own memory through the cross-process API (pid = self).
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <sys/uio.h>
#include <unistd.h>

int main(void) {
    char src[32] = "process-vm-readv-payload!!";
    char dst[32] = {0};
    struct iovec liov = {dst, sizeof src};
    struct iovec riov = {src, sizeof src};
    ssize_t n = process_vm_readv(getpid(), &liov, 1, &riov, 1, 0);
    int ok = n == (ssize_t)sizeof src && memcmp(dst, src, sizeof src) == 0;
    printf("process_vm read=%ld ok=%d\n", (long)n, ok);
    return 0;
}

// io_uring_setup(2): establish a minimal ring and mmap the SQ; report whether the kernel offers it.
#define _GNU_SOURCE
#include <errno.h>
#include <linux/io_uring.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

int main(void) {
    struct io_uring_params p;
    __builtin_memset(&p, 0, sizeof p);
    int fd = syscall(SYS_io_uring_setup, 8, &p);
    if (fd < 0) {
        // not supported here -> report the verdict (ENOSYS/EPERM)
        printf("io_uring setup=0 sq_entries=0 mmap=0\n");
        return 0;
    }
    int sq = p.sq_entries >= 8;
    size_t sring = p.sq_off.array + p.sq_entries * sizeof(unsigned);
    void *m = mmap(NULL, sring, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_SQ_RING);
    int mapped = m != MAP_FAILED;
    if (mapped) munmap(m, sring);
    close(fd);
    printf("io_uring setup=1 sq_entries=%d mmap=%d\n", sq, mapped);
    return 0;
}

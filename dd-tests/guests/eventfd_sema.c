// eventfd in EFD_SEMAPHORE mode: write a count of 5, then each read returns exactly 1 and
// decrements, so five reads drain it. This is the semaphore contract (distinct from the plain
// counter mode). Linux-specific. Diffed against a native Linux oracle.
#include <stdint.h>
#include <stdio.h>
#include <sys/eventfd.h>
#include <unistd.h>

int main(void) {
    int fd = eventfd(0, EFD_SEMAPHORE);
    if (fd < 0) { perror("eventfd"); return 1; }
    uint64_t five = 5;
    write(fd, &five, sizeof five);
    long got = 0;
    for (int i = 0; i < 5; i++) {
        uint64_t v = 0;
        if (read(fd, &v, sizeof v) == sizeof v) got += (long)v; // each read returns 1
    }
    close(fd);
    printf("eventfd_sema got=%ld\n", got); // 5
    return 0;
}

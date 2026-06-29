// timerfd: arm a single 40ms one-shot timer, block on read(), and confirm exactly one expiration
// is reported. Linux-specific (macOS uses kqueue timers, so this is emulated under the runtime).
// Diffed against a native Linux oracle.
#include <stdint.h>
#include <stdio.h>
#include <sys/timerfd.h>
#include <unistd.h>

int main(void) {
    int fd = timerfd_create(CLOCK_MONOTONIC, 0);
    if (fd < 0) { perror("timerfd_create"); return 1; }
    struct itimerspec its = {0};
    its.it_value.tv_sec = 0;
    its.it_value.tv_nsec = 40 * 1000 * 1000; // 40ms one-shot
    timerfd_settime(fd, 0, &its, NULL);

    uint64_t expirations = 0;
    ssize_t n = read(fd, &expirations, sizeof expirations); // blocks until it fires
    close(fd);
    printf("timerfd n=%ld expirations=%llu\n", (long)n, (unsigned long long)expirations); // 8 1
    return 0;
}

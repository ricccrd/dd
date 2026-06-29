// timerfd periodic: 5ms interval, three blocking reads each return >=1 expiration (deterministic count).
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <sys/timerfd.h>
#include <unistd.h>

int main(void) {
    int tfd = timerfd_create(CLOCK_MONOTONIC, 0);
    struct itimerspec its = {.it_interval = {0, 5000000}, .it_value = {0, 5000000}};
    int set = timerfd_settime(tfd, 0, &its, NULL) == 0;
    int reads = 0;
    uint64_t total = 0;
    for (int i = 0; i < 3; i++) {
        uint64_t exp = 0;
        if (read(tfd, &exp, sizeof exp) == sizeof exp && exp >= 1) { reads++; total += exp; }
    }
    close(tfd);
    printf("timerfd_interval set=%d reads=%d total_ge3=%d\n", set, reads, total >= 3); // 1 3 1
    return 0;
}

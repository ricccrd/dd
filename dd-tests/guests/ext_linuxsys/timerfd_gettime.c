// timerfd_gettime: after arming a 10s one-shot, gettime reports a positive remaining value.
#define _GNU_SOURCE
#include <stdio.h>
#include <sys/timerfd.h>
#include <unistd.h>

int main(void) {
    int tfd = timerfd_create(CLOCK_MONOTONIC, 0);
    struct itimerspec its = {.it_interval = {0, 0}, .it_value = {10, 0}};
    timerfd_settime(tfd, 0, &its, NULL);
    struct itimerspec cur;
    int rc = timerfd_gettime(tfd, &cur) == 0;
    int remaining = cur.it_value.tv_sec <= 10 && (cur.it_value.tv_sec > 0 || cur.it_value.tv_nsec > 0);
    // disarm
    struct itimerspec zero = {{0, 0}, {0, 0}};
    timerfd_settime(tfd, 0, &zero, NULL);
    timerfd_gettime(tfd, &cur);
    int disarmed = cur.it_value.tv_sec == 0 && cur.it_value.tv_nsec == 0;
    close(tfd);
    printf("timerfd_gettime rc=%d remaining=%d disarmed=%d\n", rc, remaining, disarmed);
    return 0;
}

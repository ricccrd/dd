// EFD_NONBLOCK: read on a zero counter returns EAGAIN; write then read returns the value.
#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/eventfd.h>
#include <unistd.h>

int main(void) {
    int efd = eventfd(0, EFD_NONBLOCK);
    uint64_t v = 0;
    int eagain = read(efd, &v, sizeof v) < 0 && errno == EAGAIN;
    uint64_t add = 42;
    write(efd, &add, sizeof add);
    int got = read(efd, &v, sizeof v) == sizeof v && v == 42;
    // after the read the counter is 0 again -> EAGAIN
    int eagain2 = read(efd, &v, sizeof v) < 0 && errno == EAGAIN;
    close(efd);
    printf("eventfd_nonblock eagain=%d got=%d eagain2=%d\n", eagain, got, eagain2);
    return 0;
}

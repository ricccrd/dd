// prctl PR_SET_PDEATHSIG / PR_GET_PDEATHSIG round-trip (parent-death signal).
#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <sys/prctl.h>

int main(void) {
    int set = prctl(PR_SET_PDEATHSIG, SIGTERM, 0, 0, 0) == 0;
    int sig = 0;
    int get = prctl(PR_GET_PDEATHSIG, &sig, 0, 0, 0) == 0;
    printf("prctl_pdeathsig set=%d get=%d sig=%d\n", set, get, sig == SIGTERM); // 1 1 1
    return 0;
}

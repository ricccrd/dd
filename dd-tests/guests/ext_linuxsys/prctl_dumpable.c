// prctl PR_SET_DUMPABLE / PR_GET_DUMPABLE round-trip.
#define _GNU_SOURCE
#include <stdio.h>
#include <sys/prctl.h>

int main(void) {
    prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);
    int off = prctl(PR_GET_DUMPABLE, 0, 0, 0, 0);
    prctl(PR_SET_DUMPABLE, 1, 0, 0, 0);
    int on = prctl(PR_GET_DUMPABLE, 0, 0, 0, 0);
    printf("prctl_dumpable off=%d on=%d\n", off, on); // 0 1
    return 0;
}

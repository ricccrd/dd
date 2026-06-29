// prctl PR_SET_NO_NEW_PRIVS: set the bit, read it back; once set it stays set.
#define _GNU_SOURCE
#include <stdio.h>
#include <sys/prctl.h>

int main(void) {
    int before = prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0);
    int set = prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == 0;
    int after = prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0);
    printf("prctl_nnp before=%d set=%d after=%d\n", before, set, after); // 0 1 1
    return 0;
}

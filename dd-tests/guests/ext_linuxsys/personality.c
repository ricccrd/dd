// personality(2): query (0xffffffff) returns the current persona; setting ADDR_NO_RANDOMIZE sticks.
#define _GNU_SOURCE
#include <stdio.h>
#include <sys/personality.h>

int main(void) {
    int cur = personality(0xffffffff);
    int queried = cur >= 0;
    // ADDR_NO_RANDOMIZE persona round-trips
    int set = personality(cur | ADDR_NO_RANDOMIZE);
    int now = personality(0xffffffff);
    int has_norand = now >= 0 && (now & ADDR_NO_RANDOMIZE);
    personality(cur); // restore
    printf("personality queried=%d set_ok=%d norand=%d\n", queried, set >= 0, has_norand);
    return 0;
}

// strdup / strndup / stpcpy / stpncpy — portable verdicts.
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

int main(void) {
    char *d = strdup("duplicate");
    int dup = strcmp(d, "duplicate") == 0; free(d);
    char *nd = strndup("truncated", 4);
    int ndup = strcmp(nd, "trun") == 0; free(nd);
    char buf[16];
    char *e = stpcpy(buf, "abc");
    int sp = (e == buf + 3) && strcmp(buf, "abc") == 0;
    char buf2[16]; memset(buf2, 'Z', 16);
    char *e2 = stpncpy(buf2, "xy", 5); // "xy\0\0\0", returns ptr to first nul
    int snp = (e2 == buf2 + 2) && buf2[0] == 'x' && buf2[1] == 'y'
              && buf2[2] == 0 && buf2[3] == 0 && buf2[4] == 0;
    printf("str_dup dup=%d ndup=%d stpcpy=%d stpncpy=%d\n", dup, ndup, sp, snp);
    return 0;
}

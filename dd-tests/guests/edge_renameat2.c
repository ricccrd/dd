// EDGE: renameat2 flags. RENAME_NOREPLACE must FAIL (EEXIST) when the target exists; RENAME_EXCHANGE
// must atomically SWAP two files' contents. A runtime that drops the flags (plain rename) overwrites /
// doesn't swap. Diffed vs native -> oracle.
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void put(const char *p, const char *s) { FILE *f = fopen(p, "w"); fputs(s, f); fclose(f); }
static int get(const char *p, char *b, int n) { FILE *f = fopen(p, "r"); if (!f) return -1; int k = fread(b, 1, n - 1, f); b[k] = 0; fclose(f); return k; }

int main(void) {
    const char *a = "/tmp/dd_r2_a", *b = "/tmp/dd_r2_b";
    put(a, "AAA");
    put(b, "BBB");
    // NOREPLACE over an existing target must fail with EEXIST.
    int rc1 = renameat2(AT_FDCWD, a, AT_FDCWD, b, 1 /*RENAME_NOREPLACE*/);
    int noreplace_ok = (rc1 < 0); // both files still intact
    // EXCHANGE swaps contents atomically.
    int rc2 = renameat2(AT_FDCWD, a, AT_FDCWD, b, 2 /*RENAME_EXCHANGE*/);
    char ba[8] = {0}, bb[8] = {0};
    get(a, ba, sizeof ba);
    get(b, bb, sizeof bb);
    int swapped = (rc2 == 0) && !strcmp(ba, "BBB") && !strcmp(bb, "AAA");
    unlink(a);
    unlink(b);
    printf("renameat2 noreplace=%d swapped=%d\n", noreplace_ok, swapped); // 1 1
    return 0;
}

// bsearch hits (first/middle/last) + a miss. Portable verdicts.
#include <stdio.h>
#include <stdlib.h>

static int cmp(const void *a, const void *b) {
    int x = *(const int *)a, y = *(const int *)b;
    return (x > y) - (x < y);
}

int main(void) {
    int v[] = {1, 3, 5, 7, 9, 11, 13, 15};
    int k = 7; int *r = bsearch(&k, v, 8, sizeof(int), cmp);
    int d1 = r && *r == 7 && r == v + 3;
    k = 15; r = bsearch(&k, v, 8, sizeof(int), cmp); int d2 = r && *r == 15;
    k = 1; r = bsearch(&k, v, 8, sizeof(int), cmp); int d3 = r && *r == 1;
    k = 8; r = bsearch(&k, v, 8, sizeof(int), cmp); int d4 = r == NULL;
    printf("bsearch d1=%d d2=%d d3=%d d4=%d\n", d1, d2, d3, d4);
    return 0;
}

// qsort of ints with a three-way comparator. Portable verdicts.
#include <stdio.h>
#include <stdlib.h>

static int cmp(const void *a, const void *b) {
    int x = *(const int *)a, y = *(const int *)b;
    return (x > y) - (x < y);
}

int main(void) {
    int v[] = {5, 3, 8, 1, 9, 2, 7, 4, 6, 0};
    qsort(v, 10, sizeof(int), cmp);
    int sorted = 1; for (int i = 0; i < 10; i++) if (v[i] != i) sorted = 0;
    int desc[] = {3, 1, 2}; qsort(desc, 3, sizeof(int), cmp);
    int d2 = desc[0] == 1 && desc[1] == 2 && desc[2] == 3;
    printf("qsort_int sorted=%d d2=%d\n", sorted, d2);
    return 0;
}

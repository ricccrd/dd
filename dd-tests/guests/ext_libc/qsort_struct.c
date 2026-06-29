// qsort of structs by integer key. Portable verdicts.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct rec { int key; const char *name; };
static int cmp(const void *a, const void *b) {
    return ((const struct rec *)a)->key - ((const struct rec *)b)->key;
}

int main(void) {
    struct rec r[] = {{3, "c"}, {1, "a"}, {2, "b"}, {5, "e"}, {4, "d"}};
    qsort(r, 5, sizeof r[0], cmp);
    char out[16] = {0}; for (int i = 0; i < 5; i++) strcat(out, r[i].name);
    int d1 = strcmp(out, "abcde") == 0;
    int d2 = r[0].key == 1 && r[4].key == 5;
    printf("qsort_struct d1=%d d2=%d\n", d1, d2);
    return 0;
}

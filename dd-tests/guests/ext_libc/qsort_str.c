// qsort of string pointers via strcmp comparator. Portable verdicts.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int cmp(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

int main(void) {
    const char *s[] = {"banana", "apple", "cherry", "date"};
    qsort(s, 4, sizeof(char *), cmp);
    int d1 = strcmp(s[0], "apple") == 0 && strcmp(s[3], "date") == 0;
    int d2 = strcmp(s[1], "banana") == 0 && strcmp(s[2], "cherry") == 0;
    printf("qsort_str d1=%d d2=%d\n", d1, d2);
    return 0;
}

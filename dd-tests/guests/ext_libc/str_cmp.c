// string.h comparison family incl case-insensitive + C-locale collation — portable verdicts.
#include <stdio.h>
#include <string.h>
#include <strings.h>

int main(void) {
    int eq = strcmp("abc", "abc") == 0;
    int lt = strcmp("abc", "abd") < 0;
    int gt = strcmp("abd", "abc") > 0;
    int n = strncmp("abcXX", "abcYY", 3) == 0;
    int m = memcmp("abc", "abd", 3) < 0;
    int ci = strcasecmp("Hello", "hello") == 0;
    int nci = strncasecmp("HELLOxx", "helloyy", 5) == 0;
    int coll = strcoll("abc", "abd") < 0; // C locale == strcmp ordering
    printf("str_cmp eq=%d lt=%d gt=%d n=%d m=%d ci=%d nci=%d coll=%d\n",
           eq, lt, gt, n, m, ci, nci, coll);
    return 0;
}

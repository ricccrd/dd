// POSIX regular expressions: compile an extended regex with a capture group, match against inputs,
// and extract the captured substring. Exercises regcomp/regexec/regfree (heavy libc, lots of
// internal allocation). Portable -> all engines, golden-checked.
#include <regex.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    regex_t re;
    int rc = regcomp(&re, "^([a-z]+)-([0-9]+)$", REG_EXTENDED);
    if (rc) { printf("regex compile_fail\n"); return 1; }

    regmatch_t m[3];
    int hit = regexec(&re, "alpha-2026", 3, m, 0) == 0;
    char grp[16] = {0};
    if (hit) {
        int len = m[2].rm_eo - m[2].rm_so;
        memcpy(grp, "alpha-2026" + m[2].rm_so, len);
    }
    int miss = regexec(&re, "BadInput!", 0, NULL, 0) == REG_NOMATCH;
    regfree(&re);
    printf("regex hit=%d group=%s miss=%d\n", hit, grp, miss); // 1 2026 1
    return 0;
}

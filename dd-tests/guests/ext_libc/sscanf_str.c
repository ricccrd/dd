// sscanf string/char/scanset conversions with field width. Portable verdicts.
#include <stdio.h>
#include <string.h>

int main(void) {
    char w1[16], w2[16]; sscanf("hello world", "%s %s", w1, w2);
    int d1 = strcmp(w1, "hello") == 0 && strcmp(w2, "world") == 0;
    char c; sscanf("Q", "%c", &c); int d2 = c == 'Q';
    char set[16]; sscanf("abc123", "%[a-z]", set); int d3 = strcmp(set, "abc") == 0;
    char neg[16]; sscanf("abc 123", "%[^ ]", neg); int d4 = strcmp(neg, "abc") == 0;
    char wid[16]; sscanf("abcdef", "%3s", wid); int d5 = strcmp(wid, "abc") == 0;
    printf("sscanf_str d1=%d d2=%d d3=%d d4=%d d5=%d\n", d1, d2, d3, d4, d5);
    return 0;
}

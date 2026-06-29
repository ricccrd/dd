// string.h copy/concat/move family — verdict-style booleans, portable across all engines.
#include <stdio.h>
#include <string.h>

int main(void) {
    char b[64];
    int len = strlen("hello") == 5;
    strcpy(b, "abc");
    int cpy = strcmp(b, "abc") == 0;
    char n[8];
    memset(n, 'X', sizeof n);
    strncpy(n, "hi", 4); // "hi\0\0" then untouched 'X'
    int ncpy = n[0] == 'h' && n[1] == 'i' && n[2] == '\0' && n[3] == '\0' && n[4] == 'X';
    strcpy(b, "foo"); strcat(b, "bar");
    int cat = strcmp(b, "foobar") == 0;
    strcpy(b, "foo"); strncat(b, "barbaz", 3);
    int ncat = strcmp(b, "foobar") == 0;
    char src[] = "0123456789";
    char d[16]; memcpy(d, src, 11);
    int mcpy = strcmp(d, "0123456789") == 0;
    char m[] = "aabbcc"; memmove(m + 1, m, 4); // -> "aaabbc"
    int mmove = strncmp(m, "aaabbc", 6) == 0;
    char z[5]; memset(z, 'q', 5);
    int mset = z[0] == 'q' && z[4] == 'q';
    printf("str_copy len=%d cpy=%d ncpy=%d cat=%d ncat=%d mcpy=%d mmove=%d mset=%d\n",
           len, cpy, ncpy, cat, ncat, mcpy, mmove, mset);
    return 0;
}

// string.h search family — chr/rchr/str/pbrk/spn/cspn/memchr/nlen — portable verdicts.
#include <stdio.h>
#include <string.h>

int main(void) {
    const char *s = "hello world";
    int chr = strchr(s, 'o') == s + 4;
    int rchr = strrchr(s, 'o') == s + 7;
    int str = strstr(s, "world") == s + 6;
    int nul = strstr(s, "xyz") == NULL;
    int pbrk = strpbrk(s, "dw") == s + 6; // first 'd' or 'w' -> 'w' at index 6
    int spn = strspn("abcdef", "abc") == 3;
    int cspn = strcspn("abcdef", "de") == 3;
    int mchr = memchr(s, 'w', 11) == s + 6;
    int nlen = strnlen("hello", 3) == 3 && strnlen("hi", 10) == 2;
    printf("str_search chr=%d rchr=%d str=%d nul=%d pbrk=%d spn=%d cspn=%d mchr=%d nlen=%d\n",
           chr, rchr, str, nul, pbrk, spn, cspn, mchr, nlen);
    return 0;
}

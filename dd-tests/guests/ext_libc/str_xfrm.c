// strxfrm + strcoll/strcmp ordering agreement in the C locale — portable verdicts.
#include <stdio.h>
#include <string.h>
#include <locale.h>

int main(void) {
    setlocale(LC_ALL, "C");
    char buf[32];
    size_t n = strxfrm(buf, "hello", sizeof buf); // identity transform in C locale
    int xf = n == 5;
    int ord = (strcoll("apple", "banana") < 0) == (strcmp("apple", "banana") < 0);
    printf("str_xfrm len=%d ord=%d\n", xf, ord);
    return 0;
}

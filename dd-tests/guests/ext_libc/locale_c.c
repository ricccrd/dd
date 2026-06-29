// C locale: setlocale + localeconv decimal/thousands + strcoll==strcmp ordering. Portable verdicts.
#include <stdio.h>
#include <locale.h>
#include <string.h>

int main(void) {
    int d1 = setlocale(LC_ALL, "C") != NULL;
    struct lconv *lc = localeconv();
    int d2 = strcmp(lc->decimal_point, ".") == 0;
    int d3 = lc->thousands_sep[0] == '\0'; // empty in C locale
    int d4 = setlocale(LC_NUMERIC, NULL) != NULL;
    int d5 = (strcoll("a", "b") < 0) == (strcmp("a", "b") < 0);
    printf("locale_c d1=%d d2=%d d3=%d d4=%d d5=%d\n", d1, d2, d3, d4, d5);
    return 0;
}

// mbstowcs/wcstombs/mblen roundtrip in the C locale (ASCII). Portable verdicts.
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <string.h>
#include <locale.h>

int main(void) {
    setlocale(LC_ALL, "C");
    wchar_t w[16]; size_t n = mbstowcs(w, "hello", 16);
    int d1 = n == 5 && w[0] == L'h' && w[4] == L'o';
    char m[16]; size_t n2 = wcstombs(m, L"world", 16);
    int d2 = n2 == 5 && strcmp(m, "world") == 0;
    int d3 = mblen("a", 1) == 1;
    int d4 = wcslen(w) == 5;
    printf("mbs_wcs d1=%d d2=%d d3=%d d4=%d\n", d1, d2, d3, d4);
    return 0;
}

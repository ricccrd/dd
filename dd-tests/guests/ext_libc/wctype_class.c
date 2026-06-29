// wide ctype: iswalpha/iswdigit/iswspace/iswupper/iswlower/towupper/towlower/iswxdigit. Portable verdicts.
#include <stdio.h>
#include <wctype.h>

int main(void) {
    int d1 = iswalpha(L'a') && !iswalpha(L'5');
    int d2 = iswdigit(L'7') && !iswdigit(L'x');
    int d3 = iswspace(L' ') && !iswspace(L'a');
    int d4 = iswupper(L'A') && iswlower(L'a');
    int d5 = towupper(L'a') == L'A' && towlower(L'Z') == L'z';
    int d6 = iswalnum(L'5') && iswpunct(L'!');
    int d7 = iswxdigit(L'f') && !iswxdigit(L'g');
    printf("wctype_class d1=%d d2=%d d3=%d d4=%d d5=%d d6=%d d7=%d\n",
           d1, d2, d3, d4, d5, d6, d7);
    return 0;
}

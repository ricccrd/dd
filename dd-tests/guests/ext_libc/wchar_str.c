// wchar string ops: wcslen/wcscpy/wcscat/wcscmp/wcschr/wcsncmp/wmemcpy/wcsrchr. Portable verdicts.
#include <stdio.h>
#include <wchar.h>

int main(void) {
    int d1 = wcslen(L"hello") == 5;
    wchar_t b[16]; wcscpy(b, L"abc"); int d2 = wcscmp(b, L"abc") == 0;
    wcscat(b, L"def"); int d3 = wcscmp(b, L"abcdef") == 0;
    int d4 = wcschr(L"hello", L'l') != NULL && wcschr(L"hello", L'z') == NULL;
    int d5 = wcsncmp(L"abcXX", L"abcYY", 3) == 0;
    wchar_t w[8]; wmemcpy(w, L"12345", 6); int d6 = wcscmp(w, L"12345") == 0;
    int d7 = wcsrchr(L"banana", L'a') != NULL;
    printf("wchar_str d1=%d d2=%d d3=%d d4=%d d5=%d d6=%d d7=%d\n",
           d1, d2, d3, d4, d5, d6, d7);
    return 0;
}

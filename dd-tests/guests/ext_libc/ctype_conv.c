// toupper/tolower over full ASCII alpha range + non-alpha passthrough. Portable verdicts.
#include <stdio.h>
#include <ctype.h>

int main(void) {
    int up = 1, lo = 1;
    for (int c = 'a'; c <= 'z'; c++) if (toupper(c) != c - 32) up = 0;
    for (int c = 'A'; c <= 'Z'; c++) if (tolower(c) != c + 32) lo = 0;
    int d3 = toupper('5') == '5' && tolower('!') == '!';
    int d4 = toupper('a') == 'A' && tolower('Z') == 'z';
    printf("ctype_conv up=%d lo=%d d3=%d d4=%d\n", up, lo, d3, d4);
    return 0;
}

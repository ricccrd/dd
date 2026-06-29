// ctype classification predicates across representative chars. Portable verdicts.
#include <stdio.h>
#include <ctype.h>

int main(void) {
    int d1 = isalpha('a') && isalpha('Z') && !isalpha('5') && !isalpha(' ');
    int d2 = isdigit('7') && !isdigit('a');
    int d3 = isalnum('a') && isalnum('5') && !isalnum('!');
    int d4 = isspace(' ') && isspace('\t') && isspace('\n') && !isspace('x');
    int d5 = isupper('A') && !isupper('a');
    int d6 = islower('a') && !islower('A');
    int d7 = ispunct('!') && ispunct(',') && !ispunct('a');
    int d8 = isxdigit('f') && isxdigit('9') && !isxdigit('g');
    int d9 = iscntrl('\n') && !iscntrl('a');
    int d10 = isprint('a') && isprint(' ') && !isprint('\n');
    int d11 = isgraph('a') && !isgraph(' ');
    int d12 = isblank(' ') && isblank('\t') && !isblank('\n');
    printf("ctype_class d1=%d d2=%d d3=%d d4=%d d5=%d d6=%d d7=%d d8=%d d9=%d d10=%d d11=%d d12=%d\n",
           d1, d2, d3, d4, d5, d6, d7, d8, d9, d10, d11, d12);
    return 0;
}

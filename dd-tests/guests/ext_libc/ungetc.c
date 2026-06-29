// ungetc pushback (echoed char + a different char). Portable verdicts.
#include <stdio.h>

int main(void) {
    FILE *f = tmpfile(); fputs("ABC", f); rewind(f);
    int c = fgetc(f); int d1 = c == 'A';
    int u = ungetc(c, f); int d2 = u == 'A';
    int d3 = fgetc(f) == 'A';
    int d4 = fgetc(f) == 'B';
    ungetc('Z', f);
    int d5 = fgetc(f) == 'Z';
    int d6 = fgetc(f) == 'C';
    fclose(f);
    printf("ungetc d1=%d d2=%d d3=%d d4=%d d5=%d d6=%d\n", d1, d2, d3, d4, d5, d6);
    return 0;
}

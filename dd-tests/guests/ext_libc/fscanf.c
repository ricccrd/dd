// fscanf reading mixed types from a file, across newline whitespace. Portable verdicts.
#include <stdio.h>
#include <string.h>

int main(void) {
    FILE *f = tmpfile();
    fputs("42 3.14 hello\n100 200\n", f); rewind(f);
    int a; double b; char s[16];
    int n = fscanf(f, "%d %lf %s", &a, &b, s);
    int d1 = n == 3 && a == 42 && b > 3.13 && b < 3.15 && strcmp(s, "hello") == 0;
    int x, y; fscanf(f, "%d %d", &x, &y); int d2 = x == 100 && y == 200;
    int z; int e = fscanf(f, "%d", &z); int d3 = e == EOF;
    fclose(f);
    printf("fscanf d1=%d d2=%d d3=%d\n", d1, d2, d3);
    return 0;
}

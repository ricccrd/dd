// fgets/fputs line IO incl short-buffer split and EOF. Portable verdicts.
#include <stdio.h>
#include <string.h>

int main(void) {
    FILE *f = tmpfile();
    fputs("hello\n", f); fputs("world\n", f); fputc('!', f);
    rewind(f);
    char buf[32];
    int d1 = fgets(buf, sizeof buf, f) && strcmp(buf, "hello\n") == 0;
    fgets(buf, sizeof buf, f); int d2 = strcmp(buf, "world\n") == 0;
    int d3 = fgets(buf, sizeof buf, f) && strcmp(buf, "!") == 0;
    int d4 = fgets(buf, sizeof buf, f) == NULL;
    rewind(f); char small[4]; fgets(small, 4, f);
    int d5 = strcmp(small, "hel") == 0; // n-1 chars then NUL
    fclose(f);
    printf("fgets_fputs d1=%d d2=%d d3=%d d4=%d d5=%d\n", d1, d2, d3, d4, d5);
    return 0;
}

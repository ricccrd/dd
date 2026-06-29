// setvbuf full + line buffering, then flush and read back. Portable verdicts.
#include <stdio.h>
#include <string.h>

int main(void) {
    FILE *f = tmpfile();
    char vb[BUFSIZ];
    int d1 = setvbuf(f, vb, _IOFBF, BUFSIZ) == 0;
    fputs("buffered", f); fflush(f); rewind(f);
    char buf[16]; fgets(buf, sizeof buf, f);
    int d2 = strcmp(buf, "buffered") == 0;
    FILE *g = tmpfile(); setvbuf(g, NULL, _IOLBF, 128);
    fputs("x\n", g); fflush(g); rewind(g);
    char b2[8]; fgets(b2, sizeof b2, g);
    int d3 = strcmp(b2, "x\n") == 0;
    fclose(f); fclose(g);
    printf("setvbuf d1=%d d2=%d d3=%d\n", d1, d2, d3);
    return 0;
}

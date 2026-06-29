// fseek/ftell/rewind/fgetpos/fsetpos positioning. Portable verdicts.
#include <stdio.h>

int main(void) {
    FILE *f = tmpfile();
    fputs("0123456789", f);
    int d1 = ftell(f) == 10;
    fseek(f, 3, SEEK_SET); int d2 = ftell(f) == 3 && fgetc(f) == '3';
    fseek(f, -1, SEEK_END); int d3 = fgetc(f) == '9';
    rewind(f); int d4 = ftell(f) == 0 && fgetc(f) == '0';
    fpos_t pos; fgetpos(f, &pos);   // remember position 1
    int here = fgetc(f);            // read '1'
    fsetpos(f, &pos);               // back to 1
    int d5 = fgetc(f) == here;      // re-read '1'
    fclose(f);
    printf("fseek_ftell d1=%d d2=%d d3=%d d4=%d d5=%d\n", d1, d2, d3, d4, d5);
    return 0;
}

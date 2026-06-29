// fwrite/fread binary roundtrip + EOF/feof. Portable verdicts.
#include <stdio.h>

int main(void) {
    FILE *f = tmpfile();
    int data[10]; for (int i = 0; i < 10; i++) data[i] = i * i;
    size_t w = fwrite(data, sizeof(int), 10, f); int d1 = w == 10;
    rewind(f);
    int rd[10]; size_t r = fread(rd, sizeof(int), 10, f); int d2 = r == 10;
    int ok = 1; for (int i = 0; i < 10; i++) if (rd[i] != i * i) ok = 0;
    int d3 = ok;
    int eofc = fgetc(f); int d4 = eofc == EOF && feof(f);
    fclose(f);
    printf("fread_fwrite d1=%d d2=%d d3=%d d4=%d\n", d1, d2, d3, d4);
    return 0;
}

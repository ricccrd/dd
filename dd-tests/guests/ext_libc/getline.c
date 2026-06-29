// getline / getdelim against a tmpfile. Oracle vs native Linux.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    FILE *f = tmpfile();
    fputs("line one\nline two\nfield1:field2\n", f);
    rewind(f);
    char *line = NULL; size_t cap = 0; ssize_t r; int ln = 0;
    while ((r = getline(&line, &cap, f)) > 0) {
        printf("len=%zd [%s]", r, line);
        if (++ln == 2) break;
    }
    char *fld = NULL; size_t c2 = 0;
    ssize_t r2 = getdelim(&fld, &c2, ':', f);
    printf("delim=%zd [%s]\n", r2, fld);
    free(line); free(fld); fclose(f);
    return 0;
}

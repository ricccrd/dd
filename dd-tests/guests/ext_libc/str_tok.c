// strtok / strtok_r / strsep tokenisation — portable verdicts.
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>

int main(void) {
    char a[] = "a,b,,c";
    char out[32] = {0}; int n = 0;
    for (char *t = strtok(a, ","); t; t = strtok(NULL, ",")) { strcat(out, t); n++; }
    int tok = strcmp(out, "abc") == 0 && n == 3; // empty fields collapsed by strtok

    char b[] = "x:y:z"; char *sp; char out2[32] = {0}; int n2 = 0;
    for (char *p = strtok_r(b, ":", &sp); p; p = strtok_r(NULL, ":", &sp)) { strcat(out2, p); n2++; }
    int tokr = strcmp(out2, "xyz") == 0 && n2 == 3;

    char c[] = "1,,2"; char *cp = c; char out3[32] = {0}; int n3 = 0; char *f;
    while ((f = strsep(&cp, ",")) != NULL) { if (*f) strcat(out3, f); n3++; }
    int sep = strcmp(out3, "12") == 0 && n3 == 3; // strsep keeps empty fields -> 3

    printf("str_tok tok=%d tokr=%d sep=%d\n", tok, tokr, sep);
    return 0;
}

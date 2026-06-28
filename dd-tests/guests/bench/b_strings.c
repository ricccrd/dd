// A wc/grep-style byte scanner over generated text — counts lines, words, and a 3-char pattern.
// Stand-in for everyday text tools: tight, unpredictable per-byte branching.
#include <stdio.h>
#include <stdlib.h>
int main(void) {
    const int N = 16 * 1024 * 1024;
    char *buf = malloc(N);
    unsigned long s = 12345;
    for (int i = 0; i < N; i++) { s = s * 1103515245 + 12345; int r = (int)((s >> 16) & 31); buf[i] = r < 26 ? ('a'+r) : (r < 30 ? ' ' : '\n'); }
    buf[N-1] = '\n';
    long words = 0, lines = 0, matches = 0;
    for (int rep = 0; rep < 20; rep++) {
        int inword = 0;
        for (int i = 0; i < N; i++) {
            char c = buf[i];
            if (c == '\n') lines++;
            if (c == ' ' || c == '\n') { if (inword) { words++; inword = 0; } } else inword = 1;
            if (i + 3 <= N && buf[i] == 't' && buf[i+1] == 'h' && buf[i+2] == 'e') matches++;
        }
    }
    printf("%ld %ld %ld\n", lines, words, matches);
    free(buf);
    return 0;
}

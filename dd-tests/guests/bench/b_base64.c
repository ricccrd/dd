// base64 encode a big buffer repeatedly — bit-twiddling + table lookup + throughput.
#include <stdio.h>
#include <stdlib.h>
int main(void) {
    const int N = 4 * 1024 * 1024;
    static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    unsigned char *in = malloc(N);
    char *out = malloc(((N + 2) / 3) * 4 + 4);
    for (int i = 0; i < N; i++) in[i] = (unsigned char)(i * 73 + 13);
    unsigned long sum = 0;
    for (int rep = 0; rep < 480; rep++) {
        int o = 0;
        for (int i = 0; i < N; i += 3) {
            unsigned v = (in[i] << 16) | ((i+1 < N ? in[i+1] : 0) << 8) | (i+2 < N ? in[i+2] : 0);
            out[o++] = T[(v>>18)&63]; out[o++] = T[(v>>12)&63]; out[o++] = T[(v>>6)&63]; out[o++] = T[v&63];
        }
        sum += (unsigned char)out[(unsigned)(rep * 2654435761u) % o];
        in[rep % N]++;
    }
    printf("%lu\n", sum);
    free(in); free(out);
    return 0;
}

// Dense double matrix multiply (naive i-j-k) — FP multiply/add plus cache pressure.
#include <stdio.h>
#include <stdlib.h>
int main(void) {
    const int N = 256;
    double *A = malloc(sizeof(double)*N*N), *B = malloc(sizeof(double)*N*N), *C = malloc(sizeof(double)*N*N);
    for (int i = 0; i < N*N; i++) { A[i] = (i % 7) * 0.5 + 1.0; B[i] = (i % 5) * 0.25 + 1.0; }
    for (int rep = 0; rep < 20; rep++) {
        for (int i = 0; i < N; i++)
            for (int j = 0; j < N; j++) {
                double s = 0.0;
                for (int k = 0; k < N; k++) s += A[i*N+k] * B[k*N+j];
                C[i*N+j] = s;
            }
        B[rep % (N*N)] += C[(rep*7) % (N*N)] * 1e-12;   // feed back to avoid DCE
    }
    double sum = 0.0;
    for (int i = 0; i < N*N; i++) sum += C[i];
    printf("%.0f\n", sum);
    free(A); free(B); free(C);
    return 0;
}

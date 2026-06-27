// Floating-point heavy: a tiny N-body integrator. Doubles, sqrt, lots of FP multiply/add.
#include <stdio.h>
#include <math.h>
#define N 12
int main(void) {
    double x[N], y[N], z[N], vx[N], vy[N], vz[N], m[N];
    for (int i = 0; i < N; i++) {
        x[i] = sin(i) * 10; y[i] = cos(i) * 10; z[i] = sin(i * 0.5) * 10;
        vx[i] = vy[i] = vz[i] = 0; m[i] = 1.0 + i;
    }
    const double dt = 0.001;
    double e = 0;
    for (long step = 0; step < 1000000; step++) {
        for (int i = 0; i < N; i++)
            for (int j = i + 1; j < N; j++) {
                double dx = x[j]-x[i], dy = y[j]-y[i], dz = z[j]-z[i];
                double d2 = dx*dx + dy*dy + dz*dz + 1e-9;
                double inv = 1.0 / (d2 * sqrt(d2));
                double fi = m[j] * inv, fj = m[i] * inv;
                vx[i] += dx*fi*dt; vy[i] += dy*fi*dt; vz[i] += dz*fi*dt;
                vx[j] -= dx*fj*dt; vy[j] -= dy*fj*dt; vz[j] -= dz*fj*dt;
            }
        for (int i = 0; i < N; i++) { x[i]+=vx[i]*dt; y[i]+=vy[i]*dt; z[i]+=vz[i]*dt; }
    }
    for (int i = 0; i < N; i++) e += x[i]*x[i] + y[i]*y[i] + z[i]*z[i];
    printf("%.3f\n", e);
    return 0;
}

// Mandelbrot escape-time — double FP with a data-dependent inner loop (lots of branches).
#include <stdio.h>
int main(void) {
    const int W = 800, H = 800, MAXIT = 1000;
    long total = 0;
    for (int rep = 0; rep < 3; rep++)
        for (int py = 0; py < H; py++)
            for (int px = 0; px < W; px++) {
                double x0 = (px / (double)W) * 3.5 - 2.5, y0 = (py / (double)H) * 2.0 - 1.0;
                double x = 0, y = 0; int it = 0;
                while (x*x + y*y <= 4.0 && it < MAXIT) { double xt = x*x - y*y + x0; y = 2*x*y + y0; x = xt; it++; }
                total += it;
            }
    printf("%ld\n", total);
    return 0;
}

// fmod/remainder/remquo/fdim/fabs (exact values). Portable verdicts.
#include <stdio.h>
#include <math.h>

int main(void) {
    int d1 = fmod(10.0, 3.0) == 1.0;
    int d2 = fmod(-10.0, 3.0) == -1.0; // sign of dividend
    int d3 = remainder(10.0, 3.0) == 1.0;
    int d4 = remainder(11.0, 3.0) == -1.0; // nearest multiple is 12
    int q; double r = remquo(10.0, 3.0, &q); int d5 = r == 1.0 && q == 3;
    int d6 = fdim(5.0, 3.0) == 2.0 && fdim(3.0, 5.0) == 0.0;
    int d7 = fabs(-3.5) == 3.5;
    printf("math_mod d1=%d d2=%d d3=%d d4=%d d5=%d d6=%d d7=%d\n",
           d1, d2, d3, d4, d5, d6, d7);
    return 0;
}

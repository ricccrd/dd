// fpclassify + frexp/ldexp/modf decomposition (mantissa/exponent splitting).
#include <stdio.h>
#include <math.h>
int main(void){
  double sub=1e-310;                       // subnormal
  int c0=fpclassify(0.0), cn=fpclassify(NAN), ci=fpclassify(INFINITY), cs=fpclassify(sub), cf=fpclassify(1.5);
  int exp; double m=frexp(3145728.0,&exp);
  double ip; double fp=modf(123.456,&ip);
  double ld=ldexp(0.75,5);
  printf("c0=%d cn=%d ci=%d cs=%d cf=%d m=%.4f exp=%d fp=%.3f ip=%.0f ld=%.1f\n",
    c0,cn,ci,cs,cf, m, exp, fp, ip, ld);
  return 0;
}

// long double arithmetic (80-bit x87 on x86_64, 128-bit on aarch64; oracle is per-arch).
#include <stdio.h>
#include <math.h>
int main(void){
  long double s=0.0L, p=1.0L;
  for(int i=1;i<=2000;i++){ s += 1.0L/(long double)i; p *= (1.0L + 1.0L/(long double)(i*i)); }
  long double e=expl(1.0L), pi=4.0L*atanl(1.0L);
  printf("s=%.10Lf p=%.10Lf e=%.10Lf pi=%.12Lf\n", s, p, e, pi);
  return 0;
}

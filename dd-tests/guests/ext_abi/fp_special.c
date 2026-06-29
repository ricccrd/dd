// NaN / +-inf / -0 detection and propagation; classification predicates.
#include <stdio.h>
#include <math.h>
int main(void){
  double inf=INFINITY, ninf=-INFINITY, nan=NAN, nz=-0.0;
  int a=isnan(nan), b=isinf(inf), c=signbit(nz), d=isnan(inf-inf);
  int e=isgreater(1.0,nan), f=isunordered(nan,1.0);
  double r1=inf+1.0, r2=inf-inf, r3=1.0/0.0, r4=nan+5.0;
  printf("isnan=%d isinf=%d sbz=%d infdiff=%d gt=%d uno=%d r1inf=%d r2nan=%d r3inf=%d r4nan=%d nz=%d\n",
    a,b,c,d,e,f, isinf(r1), isnan(r2), isinf(r3), isnan(r4), signbit(nz));
  return 0;
}

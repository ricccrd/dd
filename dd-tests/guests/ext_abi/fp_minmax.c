// fmin/fmax/fdim/copysign/fabs + NaN-quieting min/max semantics.
#include <stdio.h>
#include <math.h>
int main(void){
  double acc=0;
  for(int i=-100;i<100;i++){
    double a=i*1.5, b=50.0-i;
    acc += fmin(a,b) + fmax(a,b) + fdim(a,b) + copysign(2.0,a) + fabs(a);
  }
  double mn=fmin(1.0,NAN), mx=fmax(NAN,2.0);     // NaN is ignored
  printf("acc=%.4f mn=%.1f mx=%.1f cs=%.1f\n", acc, mn, mx, copysign(3.0,-0.0));
  return 0;
}

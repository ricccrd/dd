// fused multiply-add (fma/fmaf) — single rounding must match the host's hardware FMA.
#include <stdio.h>
#include <math.h>
int main(void){
  double acc=0; float facc=0;
  for(int i=1;i<=2000;i++){
    double x=i*0.001, y=i*0.002, z=i*0.0005;
    acc += fma(x,y,z);
    facc += fmaf((float)x,(float)y,(float)z);
  }
  double r=fma(1e16,1.0,1.0)-1e16;   // exposes single- vs double-rounding
  printf("acc=%.6f facc=%.4f r=%.1f\n", acc, (double)facc, r);
  return 0;
}

// interleaved int/float parameters — independent INTEGER vs SSE register sequencing.
#include <stdio.h>
static double f(int a,double b,long c,float d,int e,double g,short h,float k,long m,double n){
  return a + b + c + (double)d + e + g + h + (double)k + m + n;
}
int main(void){
  double acc=0;
  for(int i=0;i<5000;i++) acc += f(i, i*1.5, (long)i*2, (float)i*0.5f, i+1, i*2.5, (short)(i%100), (float)i*0.25f, (long)i*3, i*3.5);
  printf("acc=%.2f\n", acc);
  return 0;
}

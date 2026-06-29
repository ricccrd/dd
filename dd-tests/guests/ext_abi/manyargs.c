// functions with more args than there are registers — stack-passed arg ABI (both int and fp).
#include <stdio.h>
static long isum(long a,long b,long c,long d,long e,long f,long g,long h,long i,long j,long k,long l){
  return a-b+c-d+e-f+g-h+i-j+k-l;
}
static double fsum(double a,double b,double c,double d,double e,double f,double g,double h,double i,double j){
  return a+b*2+c*3+d*4+e*5+f*6+g*7+h*8+i*9+j*10;
}
int main(void){
  long s=0; double t=0;
  for(int i=0;i<5000;i++){
    s += isum(i,i+1,i+2,i+3,i+4,i+5,i+6,i+7,i+8,i+9,i+10,i+11);
    t += fsum(i,i*.5,i*.25,i*.125,i,i,i,i,i,i);
  }
  printf("s=%ld t=%.2f\n", s, t);
  return 0;
}

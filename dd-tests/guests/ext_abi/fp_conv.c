// int<->float conversions across widths, truncation toward zero, out-of-range edges.
#include <stdio.h>
int main(void){
  long acc=0; double dacc=0;
  for(int i=-1000;i<1000;i++){
    double d=i*1234.5678;
    acc += (long)d;                  // float->int truncation
    acc += (int)(float)(i*0.5f);
    dacc += (double)i + (double)(unsigned)(i*3);
  }
  unsigned long big=(unsigned long)(double)1e18;
  long neg=(long)-3.99;
  unsigned u=(unsigned)4294967295.0;
  printf("acc=%ld dacc=%.2f big=%lu neg=%ld u=%u\n", acc, dacc, big, neg, u);
  return 0;
}

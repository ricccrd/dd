// signed vs unsigned char arithmetic, promotion rules, and narrowing stores.
#include <stdio.h>
int main(void){
  signed char sacc=0; unsigned char uacc=0; long wide=0;
  for(int i=0;i<100000;i++){
    signed char s=(signed char)(i*3);     // wraps in 8-bit
    unsigned char u=(unsigned char)(i*5);
    sacc=(signed char)(sacc+s);
    uacc=(unsigned char)(uacc^u);
    wide += s + u;                          // both promote to int (s sign-extends, u zero-extends)
  }
  printf("sacc=%d uacc=%u wide=%ld\n", sacc, uacc, wide);
  return 0;
}

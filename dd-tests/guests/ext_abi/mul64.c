// 64-bit multiply: full 128-bit product via __int128, plus mul-overflow detection.
#include <stdio.h>
int main(void){
  unsigned long a=0x123456789ABCDEFUL, b=0xFEDCBA987654321UL;
  unsigned __int128 p=(unsigned __int128)a*b;
  unsigned long hi=(unsigned long)(p>>64), lo=(unsigned long)p;
  long x=-3037000500L, y=3037000500L; long prod; // near sqrt(2^63)
  int of = __builtin_mul_overflow(x,y,&prod);
  long ok; int no = __builtin_mul_overflow(1000000L,1000000L,&ok);
  printf("hi=%lx lo=%lx of=%d no=%d ok=%ld\n", hi, lo, of, no, ok);
  return 0;
}

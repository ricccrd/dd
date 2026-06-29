// a 16-target indirect-call site rotating through distinct functions (IBTC polymorphism).
#include <stdio.h>
#define F(n) static long f##n(long x){return x*n + (n^0x5A);}
F(1)F(2)F(3)F(4)F(5)F(6)F(7)F(8)F(9)F(10)F(11)F(12)F(13)F(14)F(15)F(16)
int main(void){
  long (*t[16])(long)={f1,f2,f3,f4,f5,f6,f7,f8,f9,f10,f11,f12,f13,f14,f15,f16};
  long acc=0;
  for(int i=0;i<200000;i++) acc += t[i%16]((long)i);
  printf("acc=%ld\n", acc);
  return 0;
}

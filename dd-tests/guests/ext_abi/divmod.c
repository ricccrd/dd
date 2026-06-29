// 64-bit signed/unsigned division & remainder, including negative-operand truncation toward zero.
#include <stdio.h>
int main(void){
  long n=-1000000007L, d=13;
  unsigned long un=0xFFFFFFFFFFFFFFFBUL, ud=1000003UL;
  long s=0; unsigned long us=0;
  for(int i=1;i<=50;i++){ s += (n - i*7) / (d + (i&3)); s += (n + i) % (d + (i&1)); }
  for(unsigned i=1;i<=50;i++){ us += un / (ud + i); us += un % (ud*i + 1); }
  printf("q=%ld r=%ld s=%ld us=%lu\n", n/d, n%d, s, us);
  return 0;
}

// byte-swap intrinsics at 16/32/64 + a manual host<->network style roundtrip.
#include <stdio.h>
int main(void){
  unsigned short a=0x1234; unsigned b=0x89ABCDEF; unsigned long c=0x0011223344556677UL;
  unsigned long acc=0;
  for(int i=0;i<1000;i++){
    acc += __builtin_bswap16(a+i);
    acc += __builtin_bswap32(b+i);
    acc += __builtin_bswap64(c+i);
  }
  printf("s=%x w=%x d=%lx acc=%lx\n", __builtin_bswap16(a), __builtin_bswap32(b), __builtin_bswap64(c), acc);
  return 0;
}

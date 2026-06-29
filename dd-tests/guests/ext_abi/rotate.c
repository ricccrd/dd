// rotate left/right idioms at 32- and 64-bit (compilers fold to ROR/ROL).
#include <stdio.h>
static unsigned rol32(unsigned x,int n){ return (x<<n)|(x>>((32-n)&31)); }
static unsigned long ror64(unsigned long x,int n){ return (x>>n)|(x<<((64-n)&63)); }
int main(void){
  unsigned a=0x12345678; unsigned long acc=0;
  for(int n=1;n<32;n++) acc ^= rol32(a,n);
  unsigned long b=0x0123456789ABCDEFUL, racc=0;
  for(int n=1;n<64;n++) racc += ror64(b,n);
  printf("acc=%lx racc=%lx\n", acc, racc);
  return 0;
}

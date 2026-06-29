// logical vs arithmetic shifts, variable shift amounts, 32- and 64-bit widths.
#include <stdio.h>
int main(void){
  long acc=0; unsigned long uacc=0;
  for(int i=0;i<64;i++){
    long sv=-12345678901234L; unsigned long uv=0xDEADBEEFCAFEBABEUL;
    acc ^= (sv >> (i&63));           // arithmetic (sign-propagating)
    uacc ^= (uv >> (i&63));          // logical
    uacc += (uv << (i&63));
  }
  unsigned x=0x80000001u; int neg=-1;
  printf("acc=%ld uacc=%lu xl=%u xr=%u asr=%d\n", acc, uacc, x<<3, x>>3, neg>>2);
  return 0;
}

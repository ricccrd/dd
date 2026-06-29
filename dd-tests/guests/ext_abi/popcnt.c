// popcount/clz/ctz/parity across a sweep of values (more breadth than compat/bitops).
#include <stdio.h>
int main(void){
  unsigned long pc=0,cz=0,tz=0,par=0;
  for(unsigned long i=1;i<=100000;i++){
    unsigned long v=i*2654435761UL ^ (i<<13);
    pc += __builtin_popcountll(v);
    cz += __builtin_clzll(v|1);
    tz += __builtin_ctzll(v|0x8000000000000000UL);
    par ^= __builtin_parityll(v);
  }
  printf("pc=%lu cz=%lu tz=%lu par=%lu\n", pc, cz, tz, par);
  return 0;
}

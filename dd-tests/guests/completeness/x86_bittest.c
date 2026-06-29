#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <immintrin.h>
#include <x86intrin.h>
int main(void){
  unsigned long v=0; unsigned char bit=0;
  __asm__("btsq $5,%0":"+r"(v)); __asm__("btsq $40,%0":"+r"(v));
  __asm__("btq $5,%1; setc %0":"=r"(bit):"r"(v):"cc");
  __asm__("btrq $5,%0":"+r"(v)); __asm__("btcq $1,%0":"+r"(v));
  long res=(long)(v&0xffffff)+bit; printf("bittest r=%ld\n", res); return 0; }

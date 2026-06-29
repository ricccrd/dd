#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <immintrin.h>
#include <x86intrin.h>
int main(void){
  unsigned long hi=0x1234567890ABCDEFUL, lo=0xFEDCBA0987654321UL, r1=hi, r2=lo;
  __asm__("shld $12,%1,%0":"+r"(r1):"r"(lo));
  __asm__("shrd $12,%1,%0":"+r"(r2):"r"(hi));
  printf("shld r=%lu\n", (r1 ^ r2) & 0xffffff); return 0; }

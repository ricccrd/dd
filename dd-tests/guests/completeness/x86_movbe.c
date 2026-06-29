#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <immintrin.h>
#include <x86intrin.h>
__attribute__((target("movbe"))) static long go(void){
  unsigned src=0x11223344, d; unsigned long long src8=0x1122334455667788UL, d8;
  __asm__("movbe %1,%0":"=r"(d):"m"(src));
  __asm__("movbe %1,%0":"=r"(d8):"m"(src8));
  return (long)((d + (d8 & 0xffff)) & 0xffffff); }
int main(void){ printf("movbe r=%ld\n", go()); return 0; }

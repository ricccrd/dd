#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <arm_neon.h>
#include <arm_acle.h>
static long go(void){
  unsigned r=__rbit(0x00000001u); unsigned long rl=__rbitll(0x1UL);
  unsigned rv=__rev(0x11223344u); unsigned short rh=__rev16(0x1122u);
  long c=__clz(0x00010000u) + __clzll(0x100000000UL);
  return (long)((r>>28) + (rl>>60) + (rv&0xff) + rh + c); }
int main(void){ printf("bitfield r=%ld\n", go()); return 0; }

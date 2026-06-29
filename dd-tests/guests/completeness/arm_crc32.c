#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <arm_neon.h>
#include <arm_acle.h>
__attribute__((target("+crc"))) static long go(void){
  uint32_t c=0; c=__crc32b(c,0x11); c=__crc32h(c,0x2233); c=__crc32w(c,0x44556677);
  c=__crc32d(c,0x1122334455667788UL);
  uint32_t cc=0; cc=__crc32cb(cc,0x11); cc=__crc32ch(cc,0x2233); cc=__crc32cw(cc,0x44556677);
  cc=__crc32cd(cc,0x1122334455667788UL);
  return (long)((c ^ cc) & 0xffffff); }
int main(void){ printf("crc32 r=%ld\n", go()); return 0; }

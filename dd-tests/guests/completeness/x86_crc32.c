#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <immintrin.h>
#include <x86intrin.h>
#include <nmmintrin.h>
__attribute__((target("sse4.2"))) static long go(void){
  unsigned c=0; c=_mm_crc32_u8(c,0x11); c=_mm_crc32_u16(c,0x2233);
  c=_mm_crc32_u32(c,0x44556677u); unsigned long long q=_mm_crc32_u64(0,0x1122334455667788UL);
  return (long)((c ^ (unsigned)q) & 0xffffff); }
int main(void){ printf("crc32 r=%ld\n", go()); return 0; }

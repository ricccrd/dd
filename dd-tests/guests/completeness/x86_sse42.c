#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <immintrin.h>
#include <x86intrin.h>
#include <nmmintrin.h>
__attribute__((target("sse4.2"))) static long go(void){
  __m128i a=_mm_set_epi64x(5,9), b=_mm_set_epi64x(7,3);
  __m128i g=_mm_cmpgt_epi64(a,b);
  long r=0; long long t[2]; _mm_storeu_si128((__m128i*)t,g); r+=t[0]+t[1];
  unsigned c=_mm_crc32_u32(0, 0x12345678u); r+=c&0xffff;
  __m128i s1=_mm_set_epi8('h','e','l','l','o',0,0,0,0,0,0,0,0,0,0,0);
  __m128i s2=s1; int idx=_mm_cmpistri(s1,s2,_SIDD_CMP_EQUAL_EACH); r+=idx;
  return r; }
int main(void){ printf("sse42 r=%ld\n", go()); return 0; }

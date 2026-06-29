#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <immintrin.h>
#include <x86intrin.h>
__attribute__((target("sse2"))) static long go(void){
  int buf[4] __attribute__((aligned(16)));
  _mm_stream_si32(&buf[0], 0x11); _mm_stream_si32(&buf[1], 0x22);
  __m128i v=_mm_set_epi32(1,2,3,4);
  _mm_stream_si128((__m128i*)buf, v); _mm_sfence();
  long r=0; for(int i=0;i<4;i++) r+=buf[i]; return r; }
int main(void){ printf("movnt r=%ld\n", go()); return 0; }

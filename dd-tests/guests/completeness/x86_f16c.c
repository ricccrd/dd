#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <immintrin.h>
#include <x86intrin.h>
__attribute__((target("f16c,avx"))) static long go(void){
  __m128 f=_mm_set_ps(1.5f,2.5f,3.5f,4.5f);
  __m128i h=_mm_cvtps_ph(f, 0); __m128 back=_mm_cvtph_ps(h);
  float o[4]; _mm_storeu_ps(o, back); long r=0; for(int i=0;i<4;i++) r+=(long)(o[i]*10); return r; }
int main(void){ printf("f16c r=%ld\n", go()); return 0; }

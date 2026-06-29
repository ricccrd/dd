#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <immintrin.h>
#include <x86intrin.h>
#include <pmmintrin.h>
__attribute__((target("sse3"))) static long go(void){
  __m128d a=_mm_set_pd(3.0,4.0), b=_mm_set_pd(1.0,2.0);
  __m128d h=_mm_hadd_pd(a,b); __m128d s=_mm_addsub_pd(a,b);
  __m128d dd=_mm_movedup_pd(a);
  return (long)(_mm_cvtsd_f64(h)*100+_mm_cvtsd_f64(s)*10+_mm_cvtsd_f64(dd)); }
int main(void){ printf("sse3 r=%ld\n", go()); return 0; }

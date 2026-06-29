#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <immintrin.h>
#include <x86intrin.h>
__attribute__((target("fma,avx"))) static long go(void){
  __m256 a=_mm256_set1_ps(3.0f), b=_mm256_set1_ps(4.0f), c=_mm256_set1_ps(5.0f);
  __m256 r=_mm256_fmadd_ps(a,b,c); __m256 r2=_mm256_fmsub_ps(a,b,c);
  __m128d d=_mm_fmadd_pd(_mm_set1_pd(2.0),_mm_set1_pd(3.0),_mm_set1_pd(1.0));
  float f[8]; long s=0; _mm256_storeu_ps(f,r); s+=(long)f[0];
  _mm256_storeu_ps(f,r2); s+=(long)f[0]; s+=(long)_mm_cvtsd_f64(d); return s; }
int main(void){ printf("fma r=%ld\n", go()); return 0; }

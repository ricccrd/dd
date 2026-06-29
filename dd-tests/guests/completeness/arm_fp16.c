#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <arm_neon.h>
__attribute__((target("+fp16"))) static long go(void){
  __fp16 a=1.5f, b=2.25f; float c=(float)a*(float)b+(float)a;
  float16x8_t va=vdupq_n_f16(1.5), vb=vdupq_n_f16(2.0);
  float16x8_t s=vaddq_f16(va,vb), m=vmulq_f16(va,vb);
  long r=(long)(c*10);
  r+=(long)(vgetq_lane_f16(s,0)*4); r+=(long)(vgetq_lane_f16(m,0)*4); return r; }
int main(void){ printf("fp16 r=%ld\n", go()); return 0; }

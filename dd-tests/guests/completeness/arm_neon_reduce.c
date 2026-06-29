#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <arm_neon.h>
static long go(void){
  int32_t va[4]={3,7,2,9}; int32x4_t a=vld1q_s32(va);
  long r=vaddvq_s32(a); r+=vmaxvq_s32(a); r+=vminvq_s32(a);
  int32x4_t p=vpaddq_s32(a,a); r+=vgetq_lane_s32(p,0)+vgetq_lane_s32(p,3);
  float32_t fa[4]={1.5,2.5,0.5,3.5}; float32x4_t f=vld1q_f32(fa);
  r+=(long)(vaddvq_f32(f)*2)+(long)vmaxvq_f32(f); return r; }
int main(void){ printf("neon_reduce r=%ld\n", go()); return 0; }

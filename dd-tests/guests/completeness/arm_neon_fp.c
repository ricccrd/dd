#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <arm_neon.h>
static long go(void){
  float32_t va[4]={1.5,2.5,3.5,4.5}; float32x4_t a=vld1q_f32(va), b=vdupq_n_f32(2.0f);
  float32x4_t s=vaddq_f32(a,b), m=vmulq_f32(a,b), f=vfmaq_f32(s,a,b), dv=vdivq_f32(a,b);
  float32_t o[4]; long r=0;
  vst1q_f32(o,s); for(int i=0;i<4;i++) r+=(long)(o[i]*2);
  vst1q_f32(o,m); for(int i=0;i<4;i++) r+=(long)(o[i]*2);
  vst1q_f32(o,f); for(int i=0;i<4;i++) r+=(long)(o[i]*2);
  vst1q_f32(o,dv); for(int i=0;i<4;i++) r+=(long)(o[i]*10);
  float64x2_t da=vdupq_n_f64(3.0), db=vdupq_n_f64(4.0); r+=(long)vgetq_lane_f64(vaddq_f64(da,db),0);
  return r; }
int main(void){ printf("neon_fp r=%ld\n", go()); return 0; }

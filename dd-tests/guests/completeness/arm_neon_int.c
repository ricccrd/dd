#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <arm_neon.h>
static long go(void){
  int32x4_t a=vdupq_n_s32(0),b; int32_t va[4]={1,2,3,4},vb[4]={5,6,7,8};
  a=vld1q_s32(va); b=vld1q_s32(vb);
  int32x4_t s=vaddq_s32(a,b), d=vsubq_s32(b,a), m=vmulq_s32(a,b), ml=vmlaq_s32(s,a,b);
  long r=0; int32_t o[4];
  vst1q_s32(o,s); for(int i=0;i<4;i++) r+=o[i];
  vst1q_s32(o,d); for(int i=0;i<4;i++) r+=o[i];
  vst1q_s32(o,m); for(int i=0;i<4;i++) r+=o[i];
  vst1q_s32(o,ml); for(int i=0;i<4;i++) r+=o[i]; return r; }
int main(void){ printf("neon_int r=%ld\n", go()); return 0; }

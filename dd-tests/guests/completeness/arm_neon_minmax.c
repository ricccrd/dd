#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <arm_neon.h>
static long go(void){
  int32_t va[4]={1,9,3,7},vb[4]={5,2,8,4}; int32x4_t a=vld1q_s32(va),b=vld1q_s32(vb);
  int32x4_t mx=vmaxq_s32(a,b), mn=vminq_s32(a,b), pmx=vpmaxq_s32(a,b);
  long r=0; int32_t o[4];
  vst1q_s32(o,mx); for(int i=0;i<4;i++) r+=o[i];
  vst1q_s32(o,mn); for(int i=0;i<4;i++) r+=o[i];
  vst1q_s32(o,pmx); for(int i=0;i<4;i++) r+=o[i];
  float32_t fa[4]={1.5,-2.5,3.5,-4.5}; float32x4_t f=vld1q_f32(fa);
  r+=(long)(vmaxnmvq_f32(f)*2); return r; }
int main(void){ printf("neon_minmax r=%ld\n", go()); return 0; }

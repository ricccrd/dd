#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <arm_neon.h>
static long go(void){
  int32_t va[4]={1,2,3,4}; int32x4_t a=vld1q_s32(va);
  int32x4_t l=vshlq_n_s32(a,2), r=vshrq_n_s32(vdupq_n_s32(-64),2);
  int32x4_t rs=vrshrq_n_s32(vdupq_n_s32(7),1);
  uint32x4_t u=vshlq_n_u32(vdupq_n_u32(1),5);
  long acc=0; int32_t o[4]; uint32_t uo[4];
  vst1q_s32(o,l); for(int i=0;i<4;i++) acc+=o[i];
  vst1q_s32(o,r); for(int i=0;i<4;i++) acc+=o[i];
  vst1q_s32(o,rs); for(int i=0;i<4;i++) acc+=o[i];
  vst1q_u32(uo,u); for(int i=0;i<4;i++) acc+=uo[i]; return acc; }
int main(void){ printf("neon_shift r=%ld\n", go()); return 0; }

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <arm_neon.h>
static long go(void){
  int32_t va[4]={1,5,3,8},vb[4]={4,5,2,9}; int32x4_t a=vld1q_s32(va),b=vld1q_s32(vb);
  uint32x4_t eq=vceqq_s32(a,b), gt=vcgtq_s32(a,b), ge=vcgeq_s32(a,b);
  int32x4_t sel=vbslq_s32(gt,a,b);
  long r=0; uint32_t o[4]; int32_t so[4];
  vst1q_u32(o,eq); for(int i=0;i<4;i++) r+=(o[i]?1:0);
  vst1q_u32(o,gt); for(int i=0;i<4;i++) r+=(o[i]?2:0);
  vst1q_u32(o,ge); for(int i=0;i<4;i++) r+=(o[i]?4:0);
  vst1q_s32(so,sel); for(int i=0;i<4;i++) r+=so[i]; return r; }
int main(void){ printf("neon_cmp r=%ld\n", go()); return 0; }

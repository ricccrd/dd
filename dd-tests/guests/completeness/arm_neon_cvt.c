#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <arm_neon.h>
static long go(void){
  float32_t fa[4]={1.7,2.2,-3.8,4.5}; float32x4_t f=vld1q_f32(fa);
  int32x4_t ti=vcvtq_s32_f32(f), ni=vcvtnq_s32_f32(f);
  int32_t ia[4]={10,20,30,40}; float32x4_t bf=vcvtq_f32_s32(vld1q_s32(ia));
  long r=0; int32_t o[4]; float32_t fo[4];
  vst1q_s32(o,ti); for(int i=0;i<4;i++) r+=o[i];
  vst1q_s32(o,ni); for(int i=0;i<4;i++) r+=o[i];
  vst1q_f32(fo,bf); for(int i=0;i<4;i++) r+=(long)fo[i]; return r; }
int main(void){ printf("neon_cvt r=%ld\n", go()); return 0; }

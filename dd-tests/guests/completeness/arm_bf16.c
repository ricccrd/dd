#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <arm_neon.h>
__attribute__((target("+bf16"))) static long go(void){
  bfloat16x8_t a=vdupq_n_bf16(vcvth_bf16_f32(1.5f)), b=vdupq_n_bf16(vcvth_bf16_f32(2.0f));
  float32x4_t acc=vdupq_n_f32(0.0f); acc=vbfdotq_f32(acc,a,b);
  long r=0; float32_t o[4]; vst1q_f32(o,acc); for(int i=0;i<4;i++) r+=(long)(o[i]*4); return r; }
int main(void){ printf("bf16 r=%ld\n", go()); return 0; }

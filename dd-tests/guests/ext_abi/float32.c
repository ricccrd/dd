// single-precision (float) math kept in 32-bit registers; transcendental + rounding.
#include <stdio.h>
#include <math.h>
int main(void){
  float s=0;
  for(int i=1;i<=1000;i++){ float x=i*0.01f; s += sinf(x)+sqrtf((float)i)+logf(x+1.0f)+cosf(x*2.0f); }
  float p=powf(2.0f,10.0f), f=fmodf(17.0f,5.0f), r=roundf(2.5f);
  printf("s=%.4f p=%.1f f=%.2f r=%.1f exp=%.4f\n", (double)s, (double)p, (double)f, (double)r, (double)expf(1.0f));
  return 0;
}

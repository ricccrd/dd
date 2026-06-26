#include <stdio.h>
#include <math.h>
int main(void){ double s=0; for(int i=1;i<=200;i++) s+=sin(i*0.01)+sqrt((double)i)+log(i+1.0)+cos(i*0.02);
  printf("s=%.6f pow=%.2f floor=%.0f fmod=%.2f\n", s, pow(2.0,12.0), floor(3.7), fmod(17.0,5.0)); return 0; }

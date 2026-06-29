#include "compat.h"
#include <stdio.h>
#include <string.h>
#include <sys/random.h>
int main(void){
  unsigned char b[32]; memset(b,0,sizeof b);
  int r=getentropy(b, sizeof b);
  int nz=0; for(int i=0;i<32;i++) if(b[i]) nz=1;
  printf("getentropy ok=%d nonzero=%d\n", r==0, r==0?nz:0); return 0; }

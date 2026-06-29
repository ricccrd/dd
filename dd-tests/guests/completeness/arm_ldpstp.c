#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <arm_neon.h>
struct pair { long a, b; };
__attribute__((noinline)) static struct pair mk(long x){ struct pair p; p.a=x; p.b=x*2; return p; }
static long go(void){
  long arr[8]; for(int i=0;i<8;i++) arr[i]=i+1;
  long dst[8]; memcpy(dst,arr,sizeof arr);   /* ldp/stp pairs */
  long r=0; for(int i=0;i<8;i++) r+=dst[i];
  struct pair p=mk(10); r+=p.a+p.b;
  double d[4]={1.5,2.5,3.5,4.5}, e[4]; memcpy(e,d,sizeof d);
  for(int i=0;i<4;i++) r+=(long)(e[i]*2); return r; }
int main(void){ printf("ldpstp r=%ld\n", go()); return 0; }

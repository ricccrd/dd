// return values of every scalar width/class — return-register selection & truncation.
#include <stdio.h>
static signed char rc(int i){ return (signed char)(i*7); }
static short rs(int i){ return (short)(i*1000); }
static int ri(int i){ return i*i - 3; }
static long rl(int i){ return (long)i*1000000000L; }
static unsigned ru(int i){ return (unsigned)(i*0x9E3779B9u); }
static float rf(int i){ return i*1.5f; }
static double rd(int i){ return i*2.25; }
static void* rp(int i){ return (void*)(long)(i*8); }
int main(void){
  long iacc=0; double facc=0; unsigned long pacc=0;
  for(int i=0;i<2000;i++){ iacc += rc(i)+rs(i)+ri(i)+rl(i)+(int)ru(i); facc += rf(i)+rd(i); pacc += (unsigned long)rp(i); }
  printf("iacc=%ld facc=%.2f pacc=%lu\n", iacc, facc, pacc);
  return 0;
}

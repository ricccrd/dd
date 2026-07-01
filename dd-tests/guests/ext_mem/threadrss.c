#include "memrss.h"
#include <pthread.h>
static void* w(void*a){ volatile long s=0; for(int i=0;i<1000;i++)s+=i; (void)a; return 0; }
int main(void){
  for(int k=0;k<8;k++){pthread_t t;pthread_create(&t,0,w,0);pthread_join(t,0);}
  long base=rss_kb();
  for(int i=0;i<512;i++){pthread_t t;if(pthread_create(&t,0,w,0)){printf("threadrss bounded=0\n");return 1;}pthread_join(t,0);}
  verdict("threadrss",base,rss_kb(),150*1024); return 0; }

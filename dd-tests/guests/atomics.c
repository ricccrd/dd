#include <stdio.h>
#include <pthread.h>
static long v=0; static void*w(void*a){ (void)a; for(int i=0;i<250000;i++) __sync_fetch_and_add(&v,1); return 0; }
int main(void){ pthread_t t[4]; for(int i=0;i<4;i++)pthread_create(&t[i],0,w,0); for(int i=0;i<4;i++)pthread_join(t[i],0);
  printf("atomic v=%ld\n",v); return 0; }

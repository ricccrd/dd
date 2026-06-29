#include "compat.h"
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
int main(void){
  struct itimerval it; memset(&it,0,sizeof it); it.it_value.tv_sec=1000;
  long s=syscall(SYS_setitimer, ITIMER_REAL, &it, (void*)0);
  struct itimerval cur; memset(&cur,0,sizeof cur);
  long g=syscall(SYS_getitimer, ITIMER_REAL, &cur);
  printf("itimer set=%d get=%d readback_pos=%d\n", s==0, g==0, cur.it_value.tv_sec>0); return 0; }

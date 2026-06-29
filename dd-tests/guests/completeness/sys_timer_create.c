#include "compat.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <signal.h>
int main(void){
  timer_t tid; struct sigevent sev; memset(&sev,0,sizeof sev); sev.sigev_notify=SIGEV_NONE;
  long c=syscall(SYS_timer_create, CLOCK_MONOTONIC, &sev, &tid);
  int gok=0, rem_pos=0, dok=0;
  if(c==0){
    struct itimerspec its; memset(&its,0,sizeof its); its.it_value.tv_sec=1000;
    syscall(SYS_timer_settime, tid, 0, &its, (void*)0);
    struct itimerspec cur; memset(&cur,0,sizeof cur);
    gok = syscall(SYS_timer_gettime, tid, &cur)==0;
    rem_pos = cur.it_value.tv_sec>0;
    dok = syscall(SYS_timer_delete, tid)==0;
  }
  printf("timer create=%d gettime=%d remaining_pos=%d delete=%d\n", c==0, gok, rem_pos, dok); return 0; }

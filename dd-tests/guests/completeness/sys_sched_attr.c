#include "compat.h"
#include <stdio.h>
#include <string.h>
#include <sched.h>
int main(void){
  long pol=syscall(SYS_sched_getscheduler, 0);
  long mx=syscall(SYS_sched_get_priority_max, SCHED_FIFO);
  long mn=syscall(SYS_sched_get_priority_min, SCHED_FIFO);
  struct sched_param sp; memset(&sp,0,sizeof sp);
  long gp=syscall(SYS_sched_getparam, 0, &sp);
  long y=syscall(SYS_sched_yield);
  printf("sched policy=%ld max=%ld min=%ld getparam=%d yield=%d\n", pol, mx, mn, gp==0, y==0); return 0; }

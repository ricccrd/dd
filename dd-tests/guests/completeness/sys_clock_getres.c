#include "compat.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
int main(void){
  struct timespec m={0,0}, r={0,0};
  long a=syscall(SYS_clock_getres, CLOCK_MONOTONIC, &m);
  long b=syscall(SYS_clock_getres, CLOCK_REALTIME, &r);
  int le_ms = (m.tv_sec==0 && m.tv_nsec>0 && m.tv_nsec<=1000000);
  printf("clock_getres mono=%d real=%d res_le_ms=%d\n", a==0, b==0, le_ms); return 0; }

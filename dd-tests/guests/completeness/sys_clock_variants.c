#include "compat.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#ifndef CLOCK_BOOTTIME
#define CLOCK_BOOTTIME 7
#endif
#ifndef CLOCK_MONOTONIC_RAW
#define CLOCK_MONOTONIC_RAW 4
#endif
int main(void){
  struct timespec t; long pr,th,bo,raw;
  pr=syscall(SYS_clock_gettime, CLOCK_PROCESS_CPUTIME_ID, &t);
  th=syscall(SYS_clock_gettime, CLOCK_THREAD_CPUTIME_ID, &t);
  bo=syscall(SYS_clock_gettime, CLOCK_BOOTTIME, &t);
  raw=syscall(SYS_clock_gettime, CLOCK_MONOTONIC_RAW, &t);
  printf("clockvar proc=%d thread=%d boot=%d raw=%d\n", pr==0, th==0, bo==0, raw==0); return 0; }

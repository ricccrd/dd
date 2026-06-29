#include "compat.h"
#include <stdio.h>
#include <string.h>
#include <sched.h>
int main(void){
  cpu_set_t set; CPU_ZERO(&set);
  long r=syscall(SYS_sched_getaffinity, 0, sizeof set, &set);
  int n = r>=0 ? CPU_COUNT(&set) : -1;
  printf("sched_affinity ok=%d ncpu_pos=%d\n", r>=0, n>0); return 0; }

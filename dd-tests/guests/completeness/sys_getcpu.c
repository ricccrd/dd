#include "compat.h"
#include <stdio.h>
#include <string.h>
int main(void){
  unsigned cpu=999, node=999;
  long r=syscall(SYS_getcpu, &cpu, &node, (void*)0);
  printf("getcpu ok=%d cpu_valid=%d\n", r==0, cpu!=999); return 0; }

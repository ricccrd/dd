#include "compat.h"
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
int main(void){
  long s=syscall(SYS_setpriority, PRIO_PROCESS, 0, 5);
  errno=0; long g=syscall(SYS_getpriority, PRIO_PROCESS, 0); /* returns 20-nice */
  printf("priority set=%d nice=%ld\n", s==0, 20-g); return 0; }

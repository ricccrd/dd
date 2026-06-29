#include "compat.h"
#include <stdio.h>
#include <string.h>
int main(void){
  long cur=syscall(SYS_setfsuid, -1);       /* query (returns current fsuid) */
  long prev=syscall(SYS_setfsuid, cur);     /* set to same (returns previous == cur) */
  long now=syscall(SYS_setfsuid, -1);
  printf("setfsuid stable=%d\n", (prev==cur && now==cur)); return 0; }

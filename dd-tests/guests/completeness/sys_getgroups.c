#include "compat.h"
#include <stdio.h>
#include <string.h>
int main(void){
  long n=syscall(SYS_getgroups, 0, (void*)0);
  printf("getgroups ret_ge0=%d\n", n>=0); return 0; }

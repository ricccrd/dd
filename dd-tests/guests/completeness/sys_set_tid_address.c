#include "compat.h"
#include <stdio.h>
#include <string.h>
int main(void){
  int tidvar=0;
  long r=syscall(SYS_set_tid_address, &tidvar);
  printf("set_tid_address pos=%d\n", r>0); return 0; }

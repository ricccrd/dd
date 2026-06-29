#include "compat.h"
#include <stdio.h>
#include <string.h>
int main(void){
  long onln=sysconf(_SC_NPROCESSORS_ONLN), conf=sysconf(_SC_NPROCESSORS_CONF);
  printf("nproc onln_pos=%d conf_pos=%d consistent=%d\n", onln>0, conf>0, onln<=conf); return 0; }

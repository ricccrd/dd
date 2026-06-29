#include "compat.h"
#include <stdio.h>
#include <string.h>
int main(void){
  uid_t r,e,s; gid_t gr,ge,gs;
  long a=syscall(SYS_getresuid, &r,&e,&s);
  long b=syscall(SYS_getresgid, &gr,&ge,&gs);
  printf("getresuid ok=%d uid_consistent=%d gid_consistent=%d\n", (a==0&&b==0), (r==e&&e==s), (gr==ge&&ge==gs)); return 0; }

#include "compat.h"
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
int main(void){
  const char *l="/tmp/ddc_lnk"; unlink(l);
  symlink("target-12345", l);
  char buf[64]; memset(buf,0,sizeof buf);
  long n=syscall(SYS_readlinkat, AT_FDCWD, l, buf, sizeof buf-1);
  printf("readlinkat n=%ld tgt=%s\n", n, n>0?buf:"?"); unlink(l); return 0; }

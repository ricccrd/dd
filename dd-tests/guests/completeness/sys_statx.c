#include "compat.h"
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
int main(void){
  const char *p="/tmp/ddc_stx"; unlink(p);
  int fd=open(p,O_CREAT|O_WRONLY|O_TRUNC,0644); write(fd,"hello",5); close(fd);
  struct statx stx; memset(&stx,0,sizeof stx);
  long r=syscall(SYS_statx, AT_FDCWD, p, 0, STATX_ALL, &stx);
  printf("statx ok=%d size=%llu isreg=%d\n", r==0, (unsigned long long)stx.stx_size, (stx.stx_mode&S_IFMT)==S_IFREG);
  unlink(p); return 0; }

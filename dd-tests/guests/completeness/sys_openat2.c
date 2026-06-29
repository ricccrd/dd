#include "compat.h"
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
struct ddc_open_how { unsigned long long flags, mode, resolve; };
int main(void){
  const char *p="/tmp/ddc_oa2"; unlink(p);
  int fd=open(p,O_CREAT|O_WRONLY|O_TRUNC,0644); write(fd,"Z",1); close(fd);
  struct ddc_open_how how; memset(&how,0,sizeof how); how.flags=O_RDONLY;
  long r=syscall(__NR_openat2, AT_FDCWD, p, &how, sizeof how);
  char b=0; int ok = r>=0; if(ok){ read((int)r,&b,1); close((int)r);} 
  printf("openat2 ok=%d byte=%c\n", ok, ok?b:'?'); unlink(p); return 0; }

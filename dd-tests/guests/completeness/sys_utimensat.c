#include "compat.h"
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
int main(void){
  const char *p="/tmp/ddc_utm"; unlink(p);
  int fd=open(p,O_CREAT|O_WRONLY,0644); close(fd);
  struct timespec ts[2]={{1000000000,0},{1000000000,0}};
  long r=syscall(SYS_utimensat, AT_FDCWD, p, ts, 0);
  struct stat st; stat(p,&st);
  printf("utimensat ok=%d mtime=%ld\n", r==0, (long)st.st_mtime); unlink(p); return 0; }

#include "memrss.h"
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
int main(void){ const size_t SZ=16u*1024*1024; const char*path="/tmp/dd_memrss.dat";
  int fd=open(path,O_RDWR|O_CREAT|O_TRUNC,0600); if(fd<0){printf("mmapfilerss bounded=0\n");return 1;}
  if(ftruncate(fd,SZ)!=0){printf("mmapfilerss bounded=0\n");return 1;}
  long base=rss_kb();
  for(int i=0;i<128;i++){void*p=mmap(0,SZ,PROT_READ,MAP_PRIVATE,fd,0);if(p==MAP_FAILED){printf("mmapfilerss bounded=0\n");return 1;}volatile char c=((char*)p)[0];(void)c;munmap(p,SZ);}
  close(fd); unlink(path);
  verdict("mmapfilerss",base,rss_kb(),150*1024); return 0; }

#include "memrss.h"
#include <fcntl.h>
#include <unistd.h>
int main(void){
  for(int k=0;k<16;k++){int fd=open("/dev/null",O_RDONLY);if(fd>=0)close(fd);}
  long base=rss_kb();
  for(int i=0;i<8000;i++){int fd=open("/dev/null",O_RDONLY);if(fd<0){printf("fdrss bounded=0\n");return 1;}close(fd);}
  verdict("fdrss",base,rss_kb(),120*1024); return 0; }

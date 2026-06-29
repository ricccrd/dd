#include "compat.h"
#include <stdio.h>
#include <string.h>
struct ddc_caphdr { unsigned version; int pid; };
struct ddc_capdata { unsigned eff, prm, inh; };
int main(void){
  struct ddc_caphdr h; h.version=0x20080522u; h.pid=0; /* _LINUX_CAPABILITY_VERSION_3 */
  struct ddc_capdata d[2]; memset(d,0,sizeof d);
  long r=syscall(SYS_capget, &h, d);
  printf("capget ok=%d\n", r==0); return 0; }

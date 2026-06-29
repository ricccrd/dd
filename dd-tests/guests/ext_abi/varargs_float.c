// many double varargs (more than the FP arg registers) — forces stack spilling of the va area.
#include <stdio.h>
#include <stdarg.h>
static double fsum(int n,...){ va_list ap; va_start(ap,n); double s=0; for(int i=0;i<n;i++) s+=va_arg(ap,double); va_end(ap); return s; }
int main(void){
  double r=fsum(12, 1.1,2.2,3.3,4.4,5.5,6.6,7.7,8.8,9.9,10.10,11.11,12.12);
  char b[64]; snprintf(b,sizeof b,"%.2f %.2f %.2f %.2f %.2f %.2f",1.0,2.0,3.0,4.0,5.0,6.0);
  printf("r=%.2f b=%s\n", r, b);
  return 0;
}

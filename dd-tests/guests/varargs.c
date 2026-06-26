#include <stdio.h>
#include <stdarg.h>
static long sum(int n,...){ va_list ap; va_start(ap,n); long s=0; for(int i=0;i<n;i++)s+=va_arg(ap,int); va_end(ap); return s; }
int main(void){ char b[64]; snprintf(b,sizeof b,"%d-%s-%x-%.2f-%c",7,"hi",255,2.5,'Z');
  printf("va=%ld fmt=%s\n", sum(5,10,20,30,40,50), b); return 0; }

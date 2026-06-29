// varargs mixing int / long / double / pointer — the trickiest ABI register-class interleaving.
#include <stdio.h>
#include <stdarg.h>
static double mix(int n,...){
  va_list ap; va_start(ap,n); double s=0;
  for(int i=0;i<n;i++){
    if(i%3==0) s += va_arg(ap,int);
    else if(i%3==1) s += va_arg(ap,double);
    else s += va_arg(ap,long);
  }
  va_end(ap); return s;
}
int main(void){
  double r=mix(9, 10, 1.5, 100L, 20, 2.5, 200L, 30, 3.5, 300L);
  char b[128]; snprintf(b,sizeof b,"%d %ld %f %p %s %c %.3e", 7, 8L, 9.5, (void*)0, "x", 'Q', 12345.678);
  printf("r=%.1f b=%s\n", r, b);
  return 0;
}

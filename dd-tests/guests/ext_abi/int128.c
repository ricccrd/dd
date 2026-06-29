// __int128 arithmetic: mul/div/mod/shift on a 128-bit type, printed as hi:lo halves.
#include <stdio.h>
static void emit(const char*tag, __int128 v){
  unsigned __int128 u=(unsigned __int128)v;
  printf("%s=%016lx%016lx ", tag,(unsigned long)(u>>64),(unsigned long)u);
}
int main(void){
  __int128 a=(__int128)0x1122334455667788LL<<32 | 0x99AABBCC;
  __int128 b=-1234567890123456789LL;
  emit("mul", a*b);
  emit("div", a/ b);
  emit("mod", a% 1000000007);
  emit("shl", a<<40);
  emit("neg", -a);
  printf("\n");
  return 0;
}

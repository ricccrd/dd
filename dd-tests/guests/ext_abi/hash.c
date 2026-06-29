// integer hashing/mixing kernels (FNV-1a + a splitmix64-style finalizer): mul/xor/shift codegen.
#include <stdio.h>
#include <string.h>
static unsigned long fnv1a(const char*s){ unsigned long h=1469598103934665603UL; while(*s){ h^=(unsigned char)*s++; h*=1099511628211UL; } return h; }
static unsigned long mix(unsigned long x){ x^=x>>30; x*=0xBF58476D1CE4E5B9UL; x^=x>>27; x*=0x94D049BB133111EBUL; x^=x>>31; return x; }
int main(void){
  const char* w[]={"the","quick","brown","fox","jumps","over","lazy","dog"};
  unsigned long h=0;
  for(int r=0;r<1000;r++) for(int i=0;i<8;i++) h = mix(h ^ fnv1a(w[i]) ^ (unsigned long)r);
  printf("h=%lx fnv=%lx\n", h, fnv1a("dd-jit"));
  return 0;
}

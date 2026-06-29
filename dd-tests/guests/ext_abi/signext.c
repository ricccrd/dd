// sign/zero extension at width boundaries + mixed-signedness comparison promotion.
#include <stdio.h>
int main(void){
  signed char sc=-5; unsigned char uc=200;
  short ss=-30000; unsigned short us=60000;
  int wi = sc + ss;                         // both sign-extended to int
  long wl = (long)sc * (long)us;            // widen char & ushort
  unsigned cmp1 = (-1 < 1u) ? 1 : 0;        // -1 promoted to unsigned -> false
  unsigned cmp2 = ((long)-1 < 1u) ? 1 : 0;  // -1 stays signed long -> true
  int z = (unsigned char)-1;                // 255
  long m = (int)0x80000000u;                // sign-extends to negative long
  printf("wi=%d wl=%ld cmp1=%u cmp2=%u z=%d m=%ld uc=%d\n", wi, wl, cmp1, cmp2, z, m, uc);
  return 0;
}

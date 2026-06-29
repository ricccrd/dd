// labels-as-values computed goto — a tiny threaded-code interpreter (indirect-branch heavy).
#include <stdio.h>
int main(void){
  static const void* tab[]={&&ADD,&&SUB,&&MUL,&&XOR,&&END};
  unsigned char prog[]={0,1,2,3,0,2,1,3,2,0,4};   // opcodes, terminated by END(4)
  long acc=1; int pc=0;
  goto *tab[prog[pc++]];
ADD: acc+=3; goto *tab[prog[pc++]];
SUB: acc-=1; goto *tab[prog[pc++]];
MUL: acc*=2; goto *tab[prog[pc++]];
XOR: acc^=5; goto *tab[prog[pc++]];
END:
  printf("acc=%ld\n", acc);
  return 0;
}

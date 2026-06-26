#include <stdio.h>
int main(void){ long s=0; for(int i=0;i<200000;i++){ switch(i&7){
  case 0:s+=1;break;case 1:s+=2;break;case 2:s+=3;break;case 3:s+=4;break;
  case 4:s-=1;break;case 5:s-=2;break;case 6:s+=i&15;break;default:s+=i&3;} } printf("switch s=%ld\n",s); return 0; }

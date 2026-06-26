#include <stdio.h>
#include <signal.h>
static volatile int got=0; static void h(int s){ got=s; }
int main(void){ signal(SIGUSR1,h); raise(SIGUSR1); signal(SIGUSR2,h); raise(SIGUSR2); printf("signal got=%d\n",got); return 0; }

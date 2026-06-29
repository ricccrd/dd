// kill(getpid(), SIGUSR2) and raise(SIGUSR1) deliver to self; SIGINFO carries the signo.
#include <signal.h>
#include <stdio.h>
#include <unistd.h>

static volatile sig_atomic_t last = 0, count = 0;
static void h(int s) { last = s; count++; }

int main(void) {
    signal(SIGUSR1, h);
    signal(SIGUSR2, h);
    raise(SIGUSR1);
    int afterraise = last == SIGUSR1;
    kill(getpid(), SIGUSR2);
    int afterkill = last == SIGUSR2;
    printf("killraise raise=%d kill=%d count=%d\n", afterraise, afterkill, count == 2);
    return 0;
}

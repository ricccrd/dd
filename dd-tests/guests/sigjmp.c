// Non-local control flow under signals: a SIGFPE/SIGSEGV-free path using sigsetjmp/siglongjmp to
// recover from a handled SIGUSR1 mid-computation. Confirms the saved signal mask is restored.
// Portable -> all engines, golden-checked.
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>

static sigjmp_buf jb;
static volatile sig_atomic_t hops;

static void handler(int sig) {
    (void)sig;
    hops++;
    siglongjmp(jb, hops); // jump back into main with value = hops
}

int main(void) {
    struct sigaction sa = {0};
    sa.sa_handler = handler;
    sigaction(SIGUSR1, &sa, NULL);

    int from = sigsetjmp(jb, 1);
    if (from < 3) {
        raise(SIGUSR1); // handler longjmps back; loop until we've hopped 3 times
        printf("unreachable\n");
    }
    printf("sigjmp hops=%d from=%d\n", hops, from); // 3 3
    return 0;
}

// SOAK: synchronous signal delivery under sustained load. A SIGUSR1 handler bumps a counter; the main
// loop does a little arithmetic work and then raise(SIGUSR1) on every one of 800,000 iterations. Because
// raise() delivers synchronously, the handler runs exactly once per iteration, so the final count is
// exactly the iteration count and the work checksum is fixed -- both deterministic. The point is to drive
// the JIT's signal machinery (deliver -> save guest context -> run the guest handler -> sigreturn ->
// resume) hundreds of thousands of times back-to-back, the regime where a context save/restore slip or a
// signal-mask leak across delivery only shows up under repeated delivery. Golden, runs on every engine.
#include <signal.h>
#include <stdint.h>
#include <stdio.h>

#define N 800000

static volatile sig_atomic_t hits;
static void on_usr1(int sig) { (void)sig; hits++; }

int main(void) {
    struct sigaction sa;
    sa.sa_handler = on_usr1;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGUSR1, &sa, 0) != 0) { printf("soak signalload sigaction_fail\n"); return 1; }
    uint64_t work = 0;
    for (long i = 0; i < N; i++) {
        work = work * 1103515245ULL + 12345ULL + (uint64_t)i; // deterministic per-iter work
        raise(SIGUSR1);                                        // synchronous -> handler runs now
    }
    printf("soak signalload hits=%d work=%llu\n", (int)hits, (unsigned long long)work);
    return 0;
}

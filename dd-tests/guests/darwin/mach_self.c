// macOS-native: mach_thread_self() must equal pthread_mach_thread_np(pthread_self()) — the Mach
// thread port for the current thread, two ways. Ties the pthread layer to Mach. darwin engine only.
#include <stdio.h>
#include <pthread.h>
#include <mach/mach.h>

int main(void) {
    mach_port_t t = mach_thread_self();
    mach_port_t t2 = pthread_mach_thread_np(pthread_self());
    int ok = (t != MACH_PORT_NULL) && (t == t2);
    mach_port_deallocate(mach_task_self(), t);
    printf("mach thread_self_match=%d\n", ok); // 1
    return 0;
}

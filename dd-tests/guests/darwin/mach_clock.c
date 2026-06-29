// macOS-native Mach clock service: host_get_clock_service(SYSTEM_CLOCK) + clock_get_time, twice,
// asserting monotonicity. The classic Mach clock RPC — no Linux equivalent. darwin engine only.
#include <stdio.h>
#include <mach/mach.h>
#include <mach/clock.h>

int main(void) {
    clock_serv_t cs;
    mach_timespec_t t1, t2;
    host_get_clock_service(mach_host_self(), SYSTEM_CLOCK, &cs);
    clock_get_time(cs, &t1);
    clock_get_time(cs, &t2);
    long a = (long)t1.tv_sec * 1000000000L + t1.tv_nsec;
    long b = (long)t2.tv_sec * 1000000000L + t2.tv_nsec;
    mach_port_deallocate(mach_task_self(), cs);
    printf("mach clock mono=%d\n", b >= a); // 1
    return 0;
}

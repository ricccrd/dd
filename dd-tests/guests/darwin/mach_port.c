// macOS-native Mach port management: allocate a RECEIVE right in our IPC space, then destroy it.
// Exercises the Mach IPC port namespace — no Linux equivalent. darwin engine only.
#include <stdio.h>
#include <mach/mach.h>

int main(void) {
    mach_port_t p = MACH_PORT_NULL;
    kern_return_t kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &p);
    int ok = (kr == KERN_SUCCESS) && (p != MACH_PORT_NULL);
    kern_return_t kr2 = mach_port_mod_refs(mach_task_self(), p, MACH_PORT_RIGHT_RECEIVE, -1);
    printf("mach port alloc=%d free=%d\n", ok, kr2 == KERN_SUCCESS); // 1 1
    return 0;
}

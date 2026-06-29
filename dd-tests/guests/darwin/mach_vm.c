// macOS-native Mach VM: vm_allocate a page (read/write), store to it, read it back, vm_deallocate.
// The Mach virtual-memory primitive underlying malloc — no Linux equivalent. darwin engine only.
#include <stdio.h>
#include <mach/mach.h>

int main(void) {
    vm_address_t addr = 0;
    kern_return_t kr = vm_allocate(mach_task_self(), &addr, 4096, VM_FLAGS_ANYWHERE);
    int ok = (kr == KERN_SUCCESS) && (addr != 0);
    if (ok) { *(volatile int *)addr = 42; ok = (*(volatile int *)addr == 42); }
    if (addr) vm_deallocate(mach_task_self(), addr, 4096);
    printf("mach vm ok=%d\n", ok); // 1
    return 0;
}

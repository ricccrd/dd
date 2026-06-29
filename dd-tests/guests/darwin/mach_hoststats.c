// macOS-native Mach host_statistics64(HOST_VM_INFO64): system VM page counts (free/active/...).
// The Mach RPC behind `vm_stat` — no Linux equivalent. darwin engine only, golden-checked.
#include <stdio.h>
#include <mach/mach.h>

int main(void) {
    vm_statistics64_data_t vm;
    mach_msg_type_number_t cnt = HOST_VM_INFO64_COUNT;
    kern_return_t kr = host_statistics64(mach_host_self(), HOST_VM_INFO64, (host_info64_t)&vm, &cnt);
    int ok = (kr == KERN_SUCCESS) && (vm.free_count > 0);
    printf("host_stats ok=%d\n", ok); // 1
    return 0;
}

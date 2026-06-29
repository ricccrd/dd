// macOS-native Mach host_info(HOST_BASIC_INFO): logical CPU count + physical memory through the Mach
// host port. The Mach RPC to the kernel host server — no Linux equivalent. darwin engine only.
#include <stdio.h>
#include <mach/mach.h>

int main(void) {
    host_basic_info_data_t info;
    mach_msg_type_number_t cnt = HOST_BASIC_INFO_COUNT;
    kern_return_t kr = host_info(mach_host_self(), HOST_BASIC_INFO, (host_info_t)&info, &cnt);
    int ok = (kr == KERN_SUCCESS) && (info.max_cpus > 0) && (info.memory_size > 0);
    printf("mach host ok=%d\n", ok); // 1
    return 0;
}

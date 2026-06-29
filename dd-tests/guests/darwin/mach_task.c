// macOS-native Mach task_info(MACH_TASK_BASIC_INFO): resident/virtual size of our own task via the
// task port. The Mach RPC to the task server — no Linux equivalent. darwin engine only.
#include <stdio.h>
#include <mach/mach.h>

int main(void) {
    mach_task_basic_info_data_t info;
    mach_msg_type_number_t cnt = MACH_TASK_BASIC_INFO_COUNT;
    kern_return_t kr = task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info, &cnt);
    int ok = (kr == KERN_SUCCESS) && (info.resident_size > 0) && (info.virtual_size > 0);
    printf("mach task ok=%d\n", ok); // 1
    return 0;
}

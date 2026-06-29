// macOS-native Mach time: mach_timebase_info (numer/denom for ns conversion) + mach_absolute_time
// monotonicity. The Mach high-resolution clock — no Linux equivalent. darwin engine only.
#include <stdio.h>
#include <stdint.h>
#include <mach/mach_time.h>

int main(void) {
    mach_timebase_info_data_t tb;
    kern_return_t kr = mach_timebase_info(&tb);
    uint64_t a = mach_absolute_time();
    uint64_t b = mach_absolute_time();
    printf("mach time tb_ok=%d mono=%d\n",
           (kr == KERN_SUCCESS && tb.numer > 0 && tb.denom > 0), b >= a); // 1 1
    return 0;
}

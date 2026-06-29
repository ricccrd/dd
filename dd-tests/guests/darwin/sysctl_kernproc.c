// macOS-native sysctl KERN_PROC/KERN_PROC_PID: ask the kernel for our own process record and confirm
// the returned pid matches getpid(). Exercises the struct-returning sysctl path. darwin engine only.
#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/sysctl.h>

int main(void) {
    struct kinfo_proc kp; size_t l = sizeof kp;
    int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid() };
    int r = sysctl(mib, 4, &kp, &l, NULL, 0);
    int ok = (r == 0) && (kp.kp_proc.p_pid == getpid());
    printf("sysctl kern_proc pid_match=%d\n", ok); // 1
    return 0;
}

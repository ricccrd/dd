// getrlimit: NOFILE and STACK have soft<=hard and positive caps; getrusage returns sane self usage.
#include <stdio.h>
#include <sys/resource.h>

int main(void) {
    struct rlimit nofile, stack;
    int r1 = getrlimit(RLIMIT_NOFILE, &nofile) == 0;
    int r2 = getrlimit(RLIMIT_STACK, &stack) == 0;
    int nofile_ok = nofile.rlim_cur <= nofile.rlim_max && nofile.rlim_cur > 0;
    int stack_ok = stack.rlim_max == RLIM_INFINITY || stack.rlim_cur <= stack.rlim_max;
    struct rusage ru;
    int ru_ok = getrusage(RUSAGE_SELF, &ru) == 0 && ru.ru_maxrss >= 0;
    printf("getrlimit nofile=%d stack=%d rusage=%d\n", nofile_ok && r1, stack_ok && r2, ru_ok);
    return 0;
}

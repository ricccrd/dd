// sysconf: page size, clk_tck, open_max, nprocessors all positive (values differ per platform -> verdicts).
#include <stdio.h>
#include <unistd.h>

int main(void) {
    long ps = sysconf(_SC_PAGESIZE);
    long clk = sysconf(_SC_CLK_TCK);
    long om = sysconf(_SC_OPEN_MAX);
    long nproc = sysconf(_SC_NPROCESSORS_ONLN);
    (void)om;
    int ok = ps > 0 && clk > 0 && nproc >= 1;
    // page size is a power of two
    int pow2 = ps > 0 && (ps & (ps - 1)) == 0;
    printf("sysconf ps=%d clk=%d ok=%d pow2=%d nproc_ge1=%d\n",
           ps > 0, clk > 0, ok, pow2, nproc >= 1);
    return 0;
}

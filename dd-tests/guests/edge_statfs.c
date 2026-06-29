// EDGE: statfs(2) must report the REAL filesystem geometry, not hardcoded constants. We print the
// block size and total/free block counts for "/tmp"; the differential oracle (native vs JIT) catches a
// runtime that returns fixed values (e.g. f_blocks=2^24, f_bfree=2^23, f_type=ext4-magic) regardless of
// the actual filesystem. Diffed vs native -> oracle.
#include <stdio.h>
#include <sys/vfs.h>

int main(void) {
    struct statfs s;
    if (statfs("/tmp", &s) < 0) { perror("statfs"); return 1; }
    // bucket the totals (exact counts vary, but hardcoded vs real differ by orders of magnitude);
    // report whether they look like the known hardcoded sentinels.
    int hardcoded = (s.f_blocks == (1 << 24)) && (s.f_bfree == (1 << 23));
    printf("statfs bsize=%ld hardcoded_sentinel=%d blocks_gt=%d\n",
           (long)s.f_bsize, hardcoded, s.f_blocks > (1 << 24));
    return 0;
}

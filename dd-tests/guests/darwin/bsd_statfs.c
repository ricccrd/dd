// macOS-native statfs: filesystem stats for "/" including f_fstypename (e.g. "apfs"). The BSD statfs
// struct layout differs from Linux statvfs. darwin engine only, golden-checked.
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>

int main(void) {
    struct statfs s;
    int r = statfs("/", &s);
    int ok = (r == 0) && (s.f_blocks > 0) && (strlen(s.f_fstypename) > 0);
    printf("statfs ok=%d type_ok=%d\n", ok, (int)(strlen(s.f_fstypename) > 0)); // 1 1
    return 0;
}

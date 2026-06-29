// macOS-native copyfile(3): the darwin file-copy API (used by Finder/cp -p, copies data + metadata).
// No Linux equivalent. Copy 5 bytes and verify the destination. darwin engine only, golden-checked.
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <copyfile.h>

int main(void) {
    char src[] = "/tmp/ddcpsrcXXXXXX";
    int fd = mkstemp(src);
    write(fd, "HELLO", 5);
    close(fd);
    char dst[] = "/tmp/ddcpdstXXXXXX";
    int fd2 = mkstemp(dst);
    close(fd2);
    int r = copyfile(src, dst, NULL, COPYFILE_DATA);
    char buf[16] = {0};
    int f = open(dst, O_RDONLY);
    int n = (f >= 0) ? (int)read(f, buf, sizeof buf) : -1;
    if (f >= 0) close(f);
    int ok = (r == 0) && (n == 5) && (memcmp(buf, "HELLO", 5) == 0);
    unlink(src);
    unlink(dst);
    printf("copyfile ok=%d\n", ok); // 1
    return 0;
}

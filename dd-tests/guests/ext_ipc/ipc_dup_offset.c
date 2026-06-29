// dup() shares the open file description (and its offset): reading via the original fd then the dup
// continues from where the first left off. Verifies shared file-offset semantics. Portable, golden.
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <stdio.h>
int main(void) {
    char path[] = "/tmp/dd_dup_XXXXXX"; int fd = mkstemp(path);
    write(fd, "0123456789", 10);
    lseek(fd, 0, SEEK_SET);
    int fd2 = dup(fd);
    char a[4] = {0}, b[4] = {0};
    read(fd, a, 3);   // "012", offset now 3 (shared)
    read(fd2, b, 3);  // "345" via the dup (same offset)
    close(fd); close(fd2); unlink(path);
    printf("dup_offset a=%s b=%s\n", a, b); // 012 345
    return 0;
}

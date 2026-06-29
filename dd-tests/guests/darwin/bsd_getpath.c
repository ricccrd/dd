// macOS-native fcntl(F_GETPATH): recover a file's path from its fd. A BSD/darwin-specific fcntl with
// no Linux equivalent (Linux uses /proc/self/fd). darwin engine only, golden-checked.
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/syslimits.h>

int main(void) {
    char path[] = "/tmp/ddgetpathXXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) { printf("fcntl getpath ok=0\n"); return 0; }
    char buf[PATH_MAX] = {0};
    int r = fcntl(fd, F_GETPATH, buf);
    char *base = strrchr(path, '/'); // /tmp -> /private/tmp on mac; match the basename
    int ok = (r == 0) && (strstr(buf, base) != NULL);
    close(fd);
    unlink(path);
    printf("fcntl getpath ok=%d\n", ok); // 1
    return 0;
}

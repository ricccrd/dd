// EDGE: /proc/self/fd — the live fd table as symlinks. Open a known file, then readlink
// /proc/self/fd/<n> and confirm it resolves back to that file; also count entries in /proc/self/fd.
// Many runtimes (and tools like lsof, and glibc's closefrom) rely on it. Verdict-checked (the exact
// path/dirfd numbers vary) so it's golden across engines.
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(void) {
    const char *path = "/tmp/dd_procfd_target";
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    char link[64], target[256] = {0};
    snprintf(link, sizeof link, "/proc/self/fd/%d", fd);
    ssize_t n = readlink(link, target, sizeof target - 1);
    int resolves = (n > 0) && (strstr(target, "dd_procfd_target") != NULL);

    DIR *d = opendir("/proc/self/fd");
    int count = 0;
    if (d) { struct dirent *e; while ((e = readdir(d))) if (e->d_name[0] != '.') count++; closedir(d); }
    close(fd);
    unlink(path);
    // at least stdin/out/err + our fd + the DIR fd are open
    printf("procfd resolves=%d enough_fds=%d\n", resolves, count >= 4); // 1 1
    return 0;
}

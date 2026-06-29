// Symlink resolution: build a 3-deep symlink chain in a temp dir, then readlink the top link and
// realpath the chain end-to-end. Prints a verdict (not absolute paths, which differ by platform —
// macOS /tmp -> /private/tmp). Exercises symlink/readlink/realpath. Portable -> all engines.
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int main(void) {
    const char *dir = "/tmp/dd_realpath_dir";
    mkdir(dir, 0755);
    char target[256], l1[256], l2[256], l3[256];
    snprintf(target, sizeof target, "%s/target.txt", dir);
    snprintf(l1, sizeof l1, "%s/link1", dir);
    snprintf(l2, sizeof l2, "%s/link2", dir);
    snprintf(l3, sizeof l3, "%s/link3", dir);
    FILE *f = fopen(target, "w");
    fputs("x", f);
    fclose(f);
    unlink(l1);
    unlink(l2);
    unlink(l3);
    symlink(target, l1);
    symlink(l1, l2);
    symlink(l2, l3);

    char rl[256] = {0};
    ssize_t n = readlink(l3, rl, sizeof rl - 1);
    int readlink_ok = (n > 0) && strstr(rl, "link2") != NULL;

    char rp[PATH_MAX] = {0};
    char *res = realpath(l3, rp);
    int realpath_ok = res && strstr(rp, "target.txt") != NULL;

    unlink(l1);
    unlink(l2);
    unlink(l3);
    unlink(target);
    rmdir(dir);
    printf("realpath readlink=%d resolve=%d\n", readlink_ok, realpath_ok); // 1 1
    return 0;
}

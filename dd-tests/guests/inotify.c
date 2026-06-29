// inotify filesystem watch: watch a temp dir for IN_CREATE, create a file, then read the
// event back and confirm the reported name. Exercises inotify_init1/inotify_add_watch and
// the variable-length event read. Deterministic -> oracle-checked.
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

int main(void) {
    const char *dir = "/tmp/dd_inotify_dir";
    mkdir(dir, 0755);
    int fd = inotify_init1(0);
    if (fd < 0) { perror("inotify_init1"); return 1; }
    int wd = inotify_add_watch(fd, dir, IN_CREATE);
    if (wd < 0) { perror("add_watch"); return 1; }

    char fpath[256];
    snprintf(fpath, sizeof fpath, "%s/created.txt", dir);
    int t = open(fpath, O_CREAT | O_WRONLY, 0644);
    close(t);

    char buf[4096];
    ssize_t n = read(fd, buf, sizeof buf);
    int matched = 0;
    for (char *p = buf; p < buf + n;) {
        struct inotify_event *e = (struct inotify_event *)p;
        if (e->len && strcmp(e->name, "created.txt") == 0 && (e->mask & IN_CREATE))
            matched = 1;
        p += sizeof(struct inotify_event) + e->len;
    }
    close(fd);
    unlink(fpath);
    rmdir(dir);
    printf("inotify matched=%d\n", matched); // 1
    return 0;
}

// inotify IN_MODIFY + IN_DELETE_SELF: watch a file, write to it, then delete it; read the events.
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

int main(void) {
    char path[128];
    snprintf(path, sizeof path, "/tmp/dd_inotify_%d", (int)getpid());
    int fd = open(path, O_CREAT | O_RDWR, 0644);
    close(fd);
    int in = inotify_init1(0);
    int wd = inotify_add_watch(in, path, IN_MODIFY | IN_ATTRIB | IN_DELETE_SELF);
    fd = open(path, O_WRONLY);
    write(fd, "data", 4);
    close(fd);
    chmod(path, 0600);
    unlink(path);
    // drain events
    char buf[4096];
    int n = read(in, buf, sizeof buf);
    int modify = 0, attrib = 0, del = 0;
    for (int o = 0; o < n;) {
        struct inotify_event *e = (struct inotify_event *)(buf + o);
        if (e->mask & IN_MODIFY) modify = 1;
        if (e->mask & IN_ATTRIB) attrib = 1;
        if (e->mask & IN_DELETE_SELF) del = 1;
        o += sizeof(struct inotify_event) + e->len;
    }
    inotify_rm_watch(in, wd);
    close(in);
    printf("inotify_modify modify=%d attrib=%d delete=%d\n", modify, attrib, del);
    return 0;
}

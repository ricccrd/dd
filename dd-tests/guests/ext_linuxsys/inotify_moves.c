// inotify on a directory: IN_CREATE then IN_MOVED_FROM/IN_MOVED_TO from a rename within it.
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

int main(void) {
    char dir[128];
    snprintf(dir, sizeof dir, "/tmp/dd_inomv_%d", (int)getpid());
    mkdir(dir, 0755);
    int in = inotify_init1(0);
    inotify_add_watch(in, dir, IN_CREATE | IN_MOVED_FROM | IN_MOVED_TO);
    char a[200], b[200];
    snprintf(a, sizeof a, "%s/a", dir);
    snprintf(b, sizeof b, "%s/b", dir);
    close(open(a, O_CREAT | O_WRONLY, 0644)); // IN_CREATE
    rename(a, b);                              // IN_MOVED_FROM + IN_MOVED_TO
    char buf[4096];
    int n = read(in, buf, sizeof buf);
    int create = 0, from = 0, to = 0;
    for (int o = 0; o < n;) {
        struct inotify_event *e = (struct inotify_event *)(buf + o);
        if (e->mask & IN_CREATE) create = 1;
        if (e->mask & IN_MOVED_FROM) from = 1;
        if (e->mask & IN_MOVED_TO) to = 1;
        o += sizeof(struct inotify_event) + e->len;
    }
    close(in);
    unlink(b);
    rmdir(dir);
    printf("inotify_moves create=%d from=%d to=%d\n", create, from, to);
    return 0;
}

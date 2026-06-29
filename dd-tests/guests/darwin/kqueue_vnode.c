// macOS-native kqueue EVFILT_VNODE: watch a file fd for NOTE_WRITE, write to it, confirm the
// notification. No Linux equivalent in this form (Linux uses inotify). darwin engine only.
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/event.h>

int main(void) {
    char path[] = "/tmp/ddkqvnodeXXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) { printf("kqueue vnode write=0\n"); return 0; }
    int kq = kqueue();
    struct kevent ch;
    EV_SET(&ch, fd, EVFILT_VNODE, EV_ADD | EV_CLEAR, NOTE_WRITE | NOTE_EXTEND, 0, NULL);
    kevent(kq, &ch, 1, NULL, 0, NULL);
    write(fd, "x", 1);
    struct kevent ev;
    struct timespec to = {1, 0};
    int n = kevent(kq, NULL, 0, &ev, 1, &to);
    int ok = (n == 1) && (ev.filter == EVFILT_VNODE) && (ev.fflags & NOTE_WRITE);
    unlink(path);
    printf("kqueue vnode write=%d\n", ok); // 1
    return 0;
}

// EDGE: file-descriptor passing over AF_UNIX via SCM_RIGHTS ancillary data — the mechanism systemd
// socket activation, Docker, and D-Bus all use. The child opens a file, sends the *fd* (not the data)
// to the parent over a socketpair; the parent reads through the received fd. If the runtime doesn't
// translate the cmsg/SCM_RIGHTS control message, the received fd is invalid and the read fails.
// Diffed vs native -> oracle.
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>

int main(void) {
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    const char *path = "/tmp/dd_scm_payload";
    pid_t pid = fork();
    if (pid == 0) {
        close(sv[0]);
        int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
        write(fd, "fd-passed-ok", 12);
        lseek(fd, 0, SEEK_SET);
        char cbuf[CMSG_SPACE(sizeof(int))];
        memset(cbuf, 0, sizeof cbuf);
        char dummy = 'x';
        struct iovec io = {&dummy, 1};
        struct msghdr mh = {0};
        mh.msg_iov = &io; mh.msg_iovlen = 1;
        mh.msg_control = cbuf; mh.msg_controllen = sizeof cbuf;
        struct cmsghdr *c = CMSG_FIRSTHDR(&mh);
        c->cmsg_level = SOL_SOCKET; c->cmsg_type = SCM_RIGHTS; c->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(c), &fd, sizeof(int));
        sendmsg(sv[1], &mh, 0);
        close(fd);
        _exit(0);
    }
    close(sv[1]);
    char cbuf[CMSG_SPACE(sizeof(int))];
    memset(cbuf, 0, sizeof cbuf);
    char dummy;
    struct iovec io = {&dummy, 1};
    struct msghdr mh = {0};
    mh.msg_iov = &io; mh.msg_iovlen = 1;
    mh.msg_control = cbuf; mh.msg_controllen = sizeof cbuf;
    recvmsg(sv[0], &mh, 0);
    int rfd = -1;
    struct cmsghdr *c = CMSG_FIRSTHDR(&mh);
    if (c && c->cmsg_type == SCM_RIGHTS) memcpy(&rfd, CMSG_DATA(c), sizeof(int));
    char buf[16] = {0};
    int n = (rfd >= 0) ? (int)read(rfd, buf, sizeof buf - 1) : -1;
    waitpid(pid, NULL, 0);
    unlink(path);
    printf("scmrights got_fd=%d data=%s\n", rfd >= 0, buf); // 1 fd-passed-ok
    return 0;
}

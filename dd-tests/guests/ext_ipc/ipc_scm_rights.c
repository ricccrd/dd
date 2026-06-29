// SCM_RIGHTS fd passing over an AF_UNIX socketpair: parent opens a temp file and sends the fd as
// ancillary data; the child reads the file through the received fd and echoes it. Portable, golden.
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>
int main(void) {
    int sv[2]; socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    pid_t pid = fork();
    if (pid == 0) {
        close(sv[0]);
        char b[1]; struct iovec io = {b, 1};
        char cbuf[CMSG_SPACE(sizeof(int))];
        struct msghdr mh = {0}; mh.msg_iov = &io; mh.msg_iovlen = 1; mh.msg_control = cbuf; mh.msg_controllen = sizeof cbuf;
        recvmsg(sv[1], &mh, 0);
        struct cmsghdr *c = CMSG_FIRSTHDR(&mh);
        int fd; memcpy(&fd, CMSG_DATA(c), sizeof(int));
        char data[64] = {0}; ssize_t n = read(fd, data, 63);
        write(sv[1], data, n > 0 ? n : 0);
        _exit(0);
    }
    close(sv[1]);
    char path[] = "/tmp/dd_scm_XXXXXX"; int fd = mkstemp(path);
    write(fd, "passed-fd-content", 17); lseek(fd, 0, SEEK_SET);
    char b[1] = "!"; struct iovec io = {b, 1};
    char cbuf[CMSG_SPACE(sizeof(int))]; memset(cbuf, 0, sizeof cbuf);
    struct msghdr mh = {0}; mh.msg_iov = &io; mh.msg_iovlen = 1; mh.msg_control = cbuf; mh.msg_controllen = sizeof cbuf;
    struct cmsghdr *c = CMSG_FIRSTHDR(&mh); c->cmsg_level = SOL_SOCKET; c->cmsg_type = SCM_RIGHTS; c->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(c), &fd, sizeof(int));
    sendmsg(sv[0], &mh, 0);
    char back[64] = {0}; read(sv[0], back, 63);
    waitpid(pid, 0, 0); close(fd); unlink(path);
    printf("scm_rights data=%s\n", back); // passed-fd-content
    return 0;
}

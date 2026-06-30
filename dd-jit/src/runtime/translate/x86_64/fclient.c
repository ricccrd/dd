// fclient.c -- tiny ddjitd client. Models a resident orchestrator (ddockerd) dispatching a launch:
//   ./fclient SOCK PROG [args...]            -- one launch, exit with the guest's code
//   ./fclient --bench N SOCK PROG [args...]  -- N internal round-trips, print median/min/p75 ms
// The whole point of the fork-server is that the *launcher* is already resident, so the marginal
// launch cost is server-fork + worker-run, NOT a fresh posix_spawn+dyld of ddjit-x86. This tiny
// binary measures exactly that marginal cost.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/un.h>

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

static int send_fds(int s, const void *buf, size_t len, const int *fds, int nfd) {
    struct iovec iov = {.iov_base = (void *)buf, .iov_len = len};
    char cbuf[CMSG_SPACE(sizeof(int) * 8)];
    memset(cbuf, 0, sizeof cbuf);
    struct msghdr mh;
    memset(&mh, 0, sizeof mh);
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    mh.msg_control = cbuf;
    mh.msg_controllen = CMSG_SPACE(sizeof(int) * nfd);
    struct cmsghdr *cm = CMSG_FIRSTHDR(&mh);
    cm->cmsg_level = SOL_SOCKET;
    cm->cmsg_type = SCM_RIGHTS;
    cm->cmsg_len = CMSG_LEN(sizeof(int) * nfd);
    memcpy(CMSG_DATA(cm), fds, sizeof(int) * nfd);
    ssize_t r;
    do { r = sendmsg(s, &mh, 0); } while (r < 0 && errno == EINTR);
    return r < 0 ? -1 : 0;
}

static size_t pack(char *out, int argc, char *const argv[]) {
    size_t o = 0;
    int32_t ac = argc;
    memcpy(out + o, &ac, 4); o += 4;
    for (int i = 0; i < argc; i++) {
        int32_t l = (int32_t)strlen(argv[i]) + 1;
        memcpy(out + o, &l, 4); o += 4;
        memcpy(out + o, argv[i], l); o += l;
    }
    return o;
}

static int one_launch(const char *sock, int argc, char *const argv[], int devnull) {
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un un;
    memset(&un, 0, sizeof un);
    un.sun_family = AF_UNIX;
    snprintf(un.sun_path, sizeof un.sun_path, "%s", sock);
    if (connect(s, (struct sockaddr *)&un, sizeof un) < 0) { perror("connect"); close(s); return -1; }
    char buf[65536];
    size_t len = pack(buf, argc, argv);
    // In bench mode redirect the guest's stdio to /dev/null so output doesn't spam; otherwise pass ours.
    int fds[3] = {devnull >= 0 ? devnull : 0, devnull >= 0 ? devnull : 1, devnull >= 0 ? devnull : 2};
    if (send_fds(s, buf, len, fds, 3) < 0) { close(s); return -1; }
    int32_t rc = 0;
    ssize_t r;
    do { r = recv(s, &rc, 4, MSG_WAITALL); } while (r < 0 && errno == EINTR);
    close(s);
    return r == 4 ? (int)rc : -1;
}

static int cmp(const void *a, const void *b) {
    double d = *(const double *)a - *(const double *)b;
    return d < 0 ? -1 : d > 0 ? 1 : 0;
}

int main(int argc, char **argv) {
    if (argc >= 2 && strcmp(argv[1], "--bench") == 0) {
        int n = atoi(argv[2]);
        const char *sock = argv[3];
        int gc = argc - 4;
        char **gv = argv + 4;
        int devnull = open("/dev/null", 1 /*O_WRONLY*/);
        for (int i = 0; i < 3; i++) one_launch(sock, gc, gv, devnull); // warmup
        double *ts = malloc(sizeof(double) * n);
        int lastrc = 0;
        for (int i = 0; i < n; i++) {
            double t0 = now_ms();
            lastrc = one_launch(sock, gc, gv, devnull);
            ts[i] = now_ms() - t0;
        }
        qsort(ts, n, sizeof(double), cmp);
        printf("median=%.3f ms  min=%.3f  p75=%.3f  max=%.3f  rc_last=%d  n=%d\n",
               ts[n / 2], ts[0], ts[(int)(n * 0.75)], ts[n - 1], lastrc, n);
        return 0;
    }
    if (argc < 3) { fprintf(stderr, "usage: fclient [--bench N] SOCK PROG [args...]\n"); return 2; }
    return one_launch(argv[1], argc - 2, argv + 2, -1);
}

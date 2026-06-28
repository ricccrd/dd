// sentry_fork -- a single-fork file round-trip that validates the untrusted-guest SENTRY split's
// CLONE-FORK lane (the riskiest, previously-untested path). With DDJIT_UNTRUSTED=1 a guest fork() is a
// real worker fork() done locally by service_local; the CHILD detects getpid()!=g_worker_pid, drops the
// inherited ring lane and CAS-claims a FRESH one (sentry_fork_child), and its forwarded fs syscalls are
// serviced by a DIFFERENT sentry thread than the parent's -- so this exercises lane-reclaim + the
// 8-ring pool under two live worker processes at once. The PARENT reaps via wait4 (owner-gated).
//
// Shape (deterministic, no timing race): the parent forks ONCE; the CHILD opens a pid-unique /tmp file,
// writes a known 256-byte buffer (openat/write/close all forwarded on the child's fresh lane) and
// _exit(7)s; the PARENT waitpid()s the child (synchronization point -> the file is fully written before
// any read), checks WIFEXITED + status 7, then opens+reads the SAME file back on ITS OWN lane and sums
// the bytes. The path carries the child pid (== fork() return, known to the parent) but the GOLDEN line
// omits it -> output is pid-independent and deterministic. The buffer is the same i*7+3 ramp as
// sentry_fs (sum over 0..255 = 32640), so the fork-lane forwarded result must equal that baseline.
//
// Registered TWICE (trusted baseline + .untrusted()) against the SAME golden line: the sentry-forwarded
// fork-lane bytes must reproduce the trusted result exactly.
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

// The deterministic payload: 256 bytes, byte i = i*7+3 (mod 256). Sum over 0..255 = 32640.
static void fill(unsigned char *buf) {
    for (int i = 0; i < 256; i++) buf[i] = (unsigned char)(i * 7 + 3);
}

// Write all `len` bytes (a forwarded write may be short -> loop). Returns 0 on success.
static int write_all(int fd, const unsigned char *buf, size_t len) {
    for (size_t off = 0; off < len;) {
        ssize_t w = write(fd, buf + off, len - off);
        if (w <= 0) return -1;
        off += (size_t)w;
    }
    return 0;
}

int main(void) {
    pid_t kid = fork();
    if (kid < 0) { perror("fork"); return 1; }

    if (kid == 0) {
        // CHILD: fresh worker process -> sentry_fork_child() drops the inherited lane, mints a new token.
        // Every fs syscall below is forwarded on the child's freshly CAS-claimed lane.
        char path[64];
        snprintf(path, sizeof path, "/tmp/sentry_fork.%d", (int)getpid());
        int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
        if (fd < 0) _exit(11);
        unsigned char buf[256];
        fill(buf);
        if (write_all(fd, buf, sizeof buf) != 0) _exit(12);
        if (close(fd) != 0) _exit(13);
        _exit(7); // distinct golden exit code the parent verifies
    }

    // PARENT: reap the child first (this synchronizes -- the child's write+close are complete before we
    // read), then read the SAME file back on the parent's own lane and verify the bytes.
    char path[64];
    snprintf(path, sizeof path, "/tmp/sentry_fork.%d", (int)kid);
    int st = 0;
    int child_exit = -1;
    if (waitpid(kid, &st, 0) == kid && WIFEXITED(st)) child_exit = WEXITSTATUS(st);

    int fd = open(path, O_RDONLY);
    unsigned long sum = 0;
    int ok = 0;
    if (fd >= 0) {
        unsigned char rd[256];
        size_t got = 0;
        while (got < sizeof rd) {
            ssize_t r = read(fd, rd + got, sizeof rd - got);
            if (r <= 0) break;
            got += (size_t)r;
        }
        for (size_t i = 0; i < got; i++) sum += rd[i];
        // verify the readback matches the deterministic payload exactly
        unsigned char want[256];
        fill(want);
        ok = (got == sizeof rd) && (memcmp(rd, want, sizeof rd) == 0);
        close(fd);
    }
    unlink(path); // cleanup (unlinkat stays worker-local; harmless with DDJIT_SANDBOX off)

    printf("sentry_fork child_exit=%d readback=%s sum=%lu\n", child_exit, ok ? "ok" : "bad", sum);
    return 0;
}

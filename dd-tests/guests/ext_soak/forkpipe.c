// SOAK: fork + pipe IPC + reap endurance (no exec -> dodges the known fork+exec gap). 2000 times:
// open a pipe, fork; the child writes a small deterministic payload and _exit()s; the parent reads it,
// folds it into a checksum, closes both ends and waitpid()s the child. This sustains thousands of cycles
// of address-space copy-on-fork + pipe fd plumbing + blocking read/write hand-off + child reaping --
// where an fd leak across fork, a copied-but-not-reset translation state, or a pipe buffer mishandled
// over a long run runs the process/fd tables dry. Deterministic checksum -> golden, runs on every engine.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void) {
    uint64_t sum = 0;
    long reaped = 0;
    for (int i = 0; i < 2000; i++) {
        int fd[2];
        if (pipe(fd) != 0) { printf("soak forkpipe pipe_fail@%d\n", i); return 1; }
        pid_t p = fork();
        if (p < 0) { printf("soak forkpipe fork_fail@%d\n", i); return 1; }
        if (p == 0) {
            close(fd[0]);
            unsigned char b[4] = { (unsigned char)i, (unsigned char)(i >> 8),
                                   (unsigned char)(i * 7), (unsigned char)(i ^ 0x5a) };
            ssize_t w = write(fd[1], b, sizeof b);
            close(fd[1]);
            _exit(w == (ssize_t)sizeof b ? 0 : 1);
        }
        close(fd[1]);
        unsigned char r[4]; size_t got = 0; ssize_t n;
        while (got < sizeof r && (n = read(fd[0], r + got, sizeof r - got)) > 0) got += (size_t)n;
        close(fd[0]);
        int st = 0;
        if (waitpid(p, &st, 0) == p && WIFEXITED(st) && WEXITSTATUS(st) == 0 && got == sizeof r) {
            sum += (uint64_t)r[0] + r[1] + r[2] + r[3];
            reaped++;
        }
    }
    printf("soak forkpipe reaped=%ld sum=%llu\n", reaped, (unsigned long long)sum);
    return 0;
}

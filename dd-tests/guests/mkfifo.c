// Named pipe (FIFO): create a FIFO, fork; the child opens it write-only and streams 500 ints,
// the parent opens read-only and sums them. Exercises mkfifo + blocking open rendezvous + the
// FIFO in the filesystem. Portable -> all engines, golden-checked.
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void) {
    const char *path = "/tmp/dd_fifo_test";
    unlink(path);
    if (mkfifo(path, 0644) < 0) { perror("mkfifo"); return 1; }
    pid_t pid = fork();
    if (pid == 0) {
        int w = open(path, O_WRONLY); // blocks until reader opens
        for (int i = 1; i <= 500; i++) write(w, &i, sizeof i);
        close(w);
        _exit(0);
    }
    int r = open(path, O_RDONLY);
    long sum = 0;
    int v;
    while (read(r, &v, sizeof v) == sizeof v) sum += v;
    close(r);
    waitpid(pid, NULL, 0);
    unlink(path);
    printf("fifo sum=%ld\n", sum); // 125250
    return 0;
}

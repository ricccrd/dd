// Two FIFOs as a request/response channel: parent sends ints 1..10 on the request FIFO, the forked
// server replies each square on the response FIFO; parent sums the squares (385). Portable, golden.
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
int main(void) {
    const char *req = "/tmp/dd_fifo_req", *rsp = "/tmp/dd_fifo_rsp";
    unlink(req); unlink(rsp); mkfifo(req, 0644); mkfifo(rsp, 0644);
    pid_t pid = fork();
    if (pid == 0) {
        int r = open(req, O_RDONLY), w = open(rsp, O_WRONLY);
        int v;
        while (read(r, &v, sizeof v) == sizeof v) { int sq = v * v; write(w, &sq, sizeof sq); if (v == 0) break; }
        close(r); close(w); _exit(0);
    }
    int w = open(req, O_WRONLY), r = open(rsp, O_RDONLY);
    long sum = 0;
    for (int i = 1; i <= 10; i++) { write(w, &i, sizeof i); int sq; read(r, &sq, sizeof sq); sum += sq; }
    int zero = 0; write(w, &zero, sizeof zero);
    close(w); close(r); waitpid(pid, 0, 0); unlink(req); unlink(rsp);
    printf("fifo_twoway sum=%ld\n", sum); // sum of squares 1..10 = 385
    return 0;
}

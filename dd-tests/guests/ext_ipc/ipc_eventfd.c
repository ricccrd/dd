// eventfd as a cross-process counter: child writes 42 then 58 and exits; parent reaps it then reads
// the accumulated counter once (==100). (Linux-only.) Diffed against the native oracle.
#include <sys/eventfd.h>
#include <stdio.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/wait.h>
int main(void) {
    int efd = eventfd(0, 0);
    pid_t pid = fork();
    if (pid == 0) { uint64_t v = 42; write(efd, &v, 8); v = 58; write(efd, &v, 8); _exit(0); }
    waitpid(pid, 0, 0);
    uint64_t got = 0; read(efd, &got, 8);
    close(efd);
    printf("eventfd sum=%lu\n", (unsigned long)got); // 100
    return 0;
}

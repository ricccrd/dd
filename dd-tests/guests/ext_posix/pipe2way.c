// two pipes across a fork for a full request/response round-trip (parent<->child).
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void) {
    int up[2], down[2]; // up: child->parent, down: parent->child
    pipe(up); pipe(down);
    pid_t c = fork();
    if (c == 0) {
        close(down[1]); close(up[0]);
        char buf[16] = {0};
        int n = read(down[0], buf, sizeof buf); // wait for request
        // reply with the byte count doubled
        int reply = n * 2;
        write(up[1], &reply, sizeof reply);
        _exit(0);
    }
    close(down[0]); close(up[1]);
    write(down[1], "ping!", 5);
    int reply = 0;
    read(up[0], &reply, sizeof reply);
    int st; waitpid(c, &st, 0);
    printf("pipe2way reply=%d reaped=%d\n", reply, WIFEXITED(st)); // 10, 1
    return 0;
}

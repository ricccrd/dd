// fd plumbing: save stdout, dup2 it onto a file, print (captured into the file), restore stdout
// via the saved fd, then read the file back. The shell-redirection pattern (`> file`). Exercises
// dup/dup2/close ordering. Portable -> all engines, golden-checked.
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(void) {
    const char *path = "/tmp/dd_dup2redir.txt";
    int saved = dup(1);
    int f = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    dup2(f, 1);
    close(f);
    write(1, "captured-line\n", 14); // goes to the file now
    fsync(1);
    dup2(saved, 1); // restore real stdout
    close(saved);

    int r = open(path, O_RDONLY);
    char buf[64] = {0};
    ssize_t n = read(r, buf, sizeof buf - 1);
    close(r);
    unlink(path);
    if (n > 0 && buf[n - 1] == '\n') buf[n - 1] = 0;
    printf("dup2 file=%s\n", buf); // captured-line
    return 0;
}

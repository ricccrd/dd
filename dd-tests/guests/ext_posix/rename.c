// rename(2): move a file; old name gone, new name has the data; overwrite an existing target.
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void put(const char *p, const char *s) {
    int fd = open(p, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    write(fd, s, strlen(s));
    close(fd);
}

int main(void) {
    char a[128], b[128];
    snprintf(a, sizeof a, "/tmp/dd_ren_a_%d", (int)getpid());
    snprintf(b, sizeof b, "/tmp/dd_ren_b_%d", (int)getpid());
    unlink(a); unlink(b);
    put(a, "from-a");
    int moved = rename(a, b) == 0;
    int oldgone = access(a, F_OK) < 0 && errno == ENOENT;
    char buf[16] = {0};
    int fd = open(b, O_RDONLY);
    int n = read(fd, buf, sizeof buf);
    close(fd);
    int newhas = n == 6 && memcmp(buf, "from-a", 6) == 0;
    // overwrite: rename onto an existing file replaces it
    put(a, "again");
    rename(a, b);
    fd = open(b, O_RDONLY);
    n = read(fd, buf, sizeof buf);
    close(fd);
    int overwrote = n == 5 && memcmp(buf, "again", 5) == 0;
    unlink(b);
    printf("rename moved=%d oldgone=%d newhas=%d overwrote=%d\n", moved, oldgone, newhas, overwrote);
    return 0;
}

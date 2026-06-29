// link/symlink/readlink: hardlink bumps st_nlink to 2 and shares data; symlink target read back.
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int main(void) {
    char a[128], h[128], s[128];
    snprintf(a, sizeof a, "/tmp/dd_lnk_%d", (int)getpid());
    snprintf(h, sizeof h, "/tmp/dd_lnk_%d.hard", (int)getpid());
    snprintf(s, sizeof s, "/tmp/dd_lnk_%d.sym", (int)getpid());
    unlink(a); unlink(h); unlink(s);
    int fd = open(a, O_CREAT | O_WRONLY, 0644);
    write(fd, "shared", 6);
    close(fd);
    int hl = link(a, h) == 0;
    struct stat st;
    stat(a, &st);
    int nlink2 = st.st_nlink == 2;
    int sl = symlink(a, s) == 0;
    char buf[160] = {0};
    ssize_t n = readlink(s, buf, sizeof buf - 1);
    int rl = n == (ssize_t)strlen(a) && strcmp(buf, a) == 0;
    // unlink the hardlink -> nlink back to 1
    unlink(h);
    stat(a, &st);
    int nlink1 = st.st_nlink == 1;
    unlink(a); unlink(s);
    printf("linksym hardlink=%d nlink2=%d symlink=%d readlink=%d nlink1=%d\n", hl, nlink2, sl, rl, nlink1);
    return 0;
}

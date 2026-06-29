// sendfile(2) Linux signature: copy a file to another fd in-kernel; offset advances.
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <unistd.h>

int main(void) {
    char a[128], b[128];
    snprintf(a, sizeof a, "/tmp/dd_sf_a_%d", (int)getpid());
    snprintf(b, sizeof b, "/tmp/dd_sf_b_%d", (int)getpid());
    int in = open(a, O_CREAT | O_RDWR | O_TRUNC, 0644);
    char data[1000];
    for (int i = 0; i < 1000; i++) data[i] = (char)(i & 0x7f);
    write(in, data, sizeof data);
    lseek(in, 0, SEEK_SET);
    int out = open(b, O_CREAT | O_RDWR | O_TRUNC, 0644);
    off_t off = 0;
    ssize_t sent = sendfile(out, in, &off, sizeof data);
    long sum = 0;
    lseek(out, 0, SEEK_SET);
    char buf[1000];
    int n = read(out, buf, sizeof buf);
    for (int i = 0; i < n; i++) sum += (unsigned char)buf[i];
    close(in); close(out);
    unlink(a); unlink(b);
    printf("sendfile sent=%ld off=%ld n=%d sum=%ld\n", (long)sent, (long)off, n, sum);
    return 0;
}

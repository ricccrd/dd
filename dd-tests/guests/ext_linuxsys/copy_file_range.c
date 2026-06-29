// copy_file_range(2): in-kernel copy between two files; offsets advance; data matches.
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(void) {
    char a[128], b[128];
    snprintf(a, sizeof a, "/tmp/dd_cfr_a_%d", (int)getpid());
    snprintf(b, sizeof b, "/tmp/dd_cfr_b_%d", (int)getpid());
    int in = open(a, O_CREAT | O_RDWR | O_TRUNC, 0644);
    char data[500];
    for (int i = 0; i < 500; i++) data[i] = (char)(i & 0x3f);
    write(in, data, sizeof data);
    int out = open(b, O_CREAT | O_RDWR | O_TRUNC, 0644);
    off_t io = 0, oo = 0;
    ssize_t c = copy_file_range(in, &io, out, &oo, sizeof data, 0);
    long sum = 0;
    lseek(out, 0, SEEK_SET);
    char buf[500];
    int n = read(out, buf, sizeof buf);
    for (int i = 0; i < n; i++) sum += (unsigned char)buf[i];
    close(in); close(out);
    unlink(a); unlink(b);
    printf("copy_file_range copied=%ld in_off=%ld out_off=%ld n=%d sum=%ld\n",
           (long)c, (long)io, (long)oo, n, sum);
    return 0;
}

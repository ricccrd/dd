// splice(2): zero-copy move of bytes from a file into a pipe and back out to another file, via the
// kernel pipe buffer. Linux-specific. Diffed against a native Linux oracle.
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(void) {
    const char *src = "/tmp/dd_splice_src", *dst = "/tmp/dd_splice_dst";
    int in = open(src, O_RDWR | O_CREAT | O_TRUNC, 0644);
    const char *payload = "splice-zero-copy-1234567890";
    write(in, payload, strlen(payload));
    lseek(in, 0, SEEK_SET);

    int p[2];
    pipe(p);
    int out = open(dst, O_RDWR | O_CREAT | O_TRUNC, 0644);
    ssize_t a = splice(in, NULL, p[1], NULL, strlen(payload), 0);
    ssize_t b = splice(p[0], NULL, out, NULL, a > 0 ? (size_t)a : 0, 0);

    lseek(out, 0, SEEK_SET);
    char buf[64] = {0};
    read(out, buf, sizeof buf - 1);
    close(in);
    close(out);
    close(p[0]);
    close(p[1]);
    unlink(src);
    unlink(dst);
    printf("splice a=%ld b=%ld data=%s\n", (long)a, (long)b, buf);
    return 0;
}

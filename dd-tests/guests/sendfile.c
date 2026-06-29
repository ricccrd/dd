// sendfile(2) zero-copy file->file transfer, plus scatter/gather readv/writev. Exercises
// sendfile offset handling and iovec syscalls. Deterministic -> oracle-checked.
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

int main(void) {
    const char *src = "/tmp/dd_sf_src.bin", *dst = "/tmp/dd_sf_dst.bin";
    int in = open(src, O_RDWR | O_CREAT | O_TRUNC, 0644);
    // build the source via writev (two iovecs)
    struct iovec iov[2] = {{"hello-", 6}, {"sendfile-payload", 16}};
    writev(in, iov, 2);
    lseek(in, 0, SEEK_SET);

    struct stat st;
    fstat(in, &st);
    int out = open(dst, O_RDWR | O_CREAT | O_TRUNC, 0644);
    off_t off = 0;
    ssize_t sent = sendfile(out, in, &off, st.st_size);

    // read back via readv into two halves
    lseek(out, 0, SEEK_SET);
    char a[6] = {0}, b[64] = {0};
    struct iovec riov[2] = {{a, 6}, {b, sizeof b - 1}};
    readv(out, riov, 2);
    close(in);
    close(out);
    unlink(src);
    unlink(dst);
    printf("sendfile sent=%ld a=%.6s b=%s\n", (long)sent, a, b);
    return 0;
}

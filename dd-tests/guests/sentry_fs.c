// sentry_fs -- a pure file-IO round-trip used to validate the untrusted-guest SENTRY split.
// Every syscall here is in the sentry's forwarded fs set (openat/write/lseek/read/pread64/fstat/
// getdents64/close), so running it with DDJIT_UNTRUSTED=1 forces each one across the worker->sentry
// ring and back. Registered TWICE (trusted baseline + .untrusted()) against the SAME golden line, so
// the marshaled+copied-back bytes must reproduce the trusted result exactly. Output is independent of
// the (pid-unique) path -> deterministic. No fork/exec: isolates the plain fs forwarding path.
#define _GNU_SOURCE
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int main(void) {
    char path[64];
    snprintf(path, sizeof path, "/tmp/sentry_fs.%d", (int)getpid());
    const char *base = strrchr(path, '/') + 1; // the leaf name we look for in the dir listing

    // openat(O_CREAT|O_RDWR) -- the sentry returns a sentry-owned fd, virtual to the worker.
    int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (fd < 0) { perror("open"); return 1; }

    // write a known 256-byte buffer (forwarded: the payload is copied INTO the ring).
    unsigned char buf[256];
    for (int i = 0; i < 256; i++) buf[i] = (unsigned char)(i * 7 + 3);
    for (size_t off = 0; off < sizeof buf;) {
        ssize_t w = write(fd, buf + off, sizeof buf - off);
        if (w <= 0) { perror("write"); return 1; }
        off += (size_t)w;
    }

    // lseek back to 0, read() the first half, pread() the second half at an explicit offset.
    if (lseek(fd, 0, SEEK_SET) != 0) { perror("lseek"); return 1; }
    unsigned char rd[256];
    size_t got = 0;
    while (got < 128) { ssize_t r = read(fd, rd + got, 128 - got); if (r <= 0) break; got += (size_t)r; }
    while (got < 256) { ssize_t r = pread(fd, rd + got, 256 - got, (off_t)got); if (r <= 0) break; got += (size_t)r; }
    unsigned long sum = 0;
    for (size_t i = 0; i < got; i++) sum += rd[i];

    // fstat the size (out-struct copied back from the ring; per-arch struct stat layout).
    struct stat st;
    if (fstat(fd, &st) != 0) { perror("fstat"); return 1; }

    // getdents64 the dir (opendir->openat+fstat+getdents64): confirm our leaf is listed.
    int found = 0;
    DIR *d = opendir("/tmp");
    if (d) {
        struct dirent *de;
        while ((de = readdir(d))) if (strcmp(de->d_name, base) == 0) { found = 1; break; }
        closedir(d);
    }

    close(fd);
    unlink(path); // cleanup (unlinkat stays worker-local; harmless with DDJIT_SANDBOX off)

    printf("sentry_fs sum=%lu size=%lld found=%d\n", sum, (long long)st.st_size, found);
    return 0;
}

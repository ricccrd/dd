// Kernel-AIO (libaio) round-trip via RAW syscalls: io_setup -> io_submit(PREAD) -> io_getevents.
// Writes a known payload to a temp file, then reads it back through the AIO path at an offset and
// verifies the completion (res == nbytes) and the bytes. Deterministic stdout -> oracle-checked: the
// native aarch64 kernel supports AIO, and the dd JIT must produce the identical result.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <linux/aio_abi.h>

static int io_setup_(unsigned n, aio_context_t *c) { return (int)syscall(SYS_io_setup, n, c); }
static int io_submit_(aio_context_t c, long n, struct iocb **p) { return (int)syscall(SYS_io_submit, c, n, p); }
static int io_getevents_(aio_context_t c, long mn, long n, struct io_event *e, struct timespec *t) {
    return (int)syscall(SYS_io_getevents, c, mn, n, e, t);
}
static int io_destroy_(aio_context_t c) { return (int)syscall(SYS_io_destroy, c); }

int main(void) {
    const char *path = "/tmp/dd_aio_test.bin";
    const char payload[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"; // 36 bytes
    size_t plen = sizeof payload - 1;

    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) { printf("open fail\n"); return 1; }
    if (write(fd, payload, plen) != (ssize_t)plen) { printf("write fail\n"); return 1; }

    aio_context_t ctx = 0;
    if (io_setup_(8, &ctx) < 0) { printf("io_setup fail\n"); return 1; }

    char buf[16];
    memset(buf, 0, sizeof buf);
    off_t off = 10; // read "KLMNOPQRST" (10 bytes from offset 10)
    size_t nb = 10;

    struct iocb cb;
    memset(&cb, 0, sizeof cb);
    cb.aio_lio_opcode = IOCB_CMD_PREAD;
    cb.aio_fildes = fd;
    cb.aio_buf = (uint64_t)(uintptr_t)buf;
    cb.aio_nbytes = nb;
    cb.aio_offset = off;
    cb.aio_data = 0xd00dfeedULL;

    struct iocb *cbs[1] = {&cb};
    int s = io_submit_(ctx, 1, cbs);
    if (s != 1) { printf("io_submit=%d\n", s); return 1; }

    struct io_event ev[1];
    memset(ev, 0, sizeof ev);
    int g = io_getevents_(ctx, 1, 1, ev, NULL);
    if (g != 1) { printf("io_getevents=%d\n", g); return 1; }

    buf[nb] = 0;
    printf("aio res=%lld data=%llx buf=%s\n", (long long)ev[0].res, (unsigned long long)ev[0].data, buf);

    io_destroy_(ctx);
    close(fd);
    unlink(path);
    return 0;
}

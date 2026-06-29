// raw getdents64(2) syscall: enumerate a directory and decode d_type from the kernel struct.
#define _GNU_SOURCE
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

struct lde { unsigned long ino, off; unsigned short reclen; unsigned char type; char name[]; };

int main(void) {
    char dir[128];
    snprintf(dir, sizeof dir, "/tmp/dd_gd64_%d", (int)getpid());
    mkdir(dir, 0755);
    char p[200];
    snprintf(p, sizeof p, "%s/reg", dir); close(open(p, O_CREAT | O_WRONLY, 0644));
    snprintf(p, sizeof p, "%s/dir", dir); mkdir(p, 0755);
    int fd = open(dir, O_RDONLY | O_DIRECTORY);
    char buf[1024];
    int regs = 0, dirs = 0, total = 0;
    for (;;) {
        long n = syscall(SYS_getdents64, fd, buf, sizeof buf);
        if (n <= 0) break;
        for (long o = 0; o < n;) {
            struct lde *d = (struct lde *)(buf + o);
            if (d->name[0] != '.') {
                total++;
                if (d->type == DT_DIR) dirs++;
                else if (d->type == DT_REG) regs++;
            }
            o += d->reclen;
        }
    }
    close(fd);
    snprintf(p, sizeof p, "%s/reg", dir); unlink(p);
    snprintf(p, sizeof p, "%s/dir", dir); rmdir(p);
    rmdir(dir);
    printf("getdents64 total=%d reg=%d dir=%d\n", total, regs, dirs);
    return 0;
}

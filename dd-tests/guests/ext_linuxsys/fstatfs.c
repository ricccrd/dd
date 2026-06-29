// statfs/fstatfs(2): block size positive, total blocks positive, free<=total; both forms agree.
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <sys/vfs.h>
#include <unistd.h>

int main(void) {
    struct statfs sp;
    int sok = statfs("/tmp", &sp) == 0;
    int fd = open("/tmp", O_RDONLY);
    struct statfs sf;
    int fok = fstatfs(fd, &sf) == 0;
    close(fd);
    int bsize = sp.f_bsize > 0;
    int blocks = sp.f_blocks > 0;
    int free_le = sp.f_bfree <= sp.f_blocks;
    int agree = sp.f_bsize == sf.f_bsize;
    printf("fstatfs statfs=%d fstatfs=%d bsize=%d blocks=%d free_le=%d agree=%d\n",
           sok, fok, bsize, blocks, free_le, agree);
    return 0;
}

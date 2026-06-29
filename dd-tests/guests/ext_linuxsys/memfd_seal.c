// memfd_create with MFD_ALLOW_SEALING; add F_SEAL_WRITE; further writes must fail EPERM.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

int main(void) {
    int fd = memfd_create("dd_seal", MFD_ALLOW_SEALING);
    int created = fd >= 0;
    write(fd, "hello", 5);
    int seal = fcntl(fd, F_ADD_SEALS, F_SEAL_WRITE) == 0;
    // writing after F_SEAL_WRITE must fail
    int denied = write(fd, "x", 1) < 0 && errno == EPERM;
    int seals = fcntl(fd, F_GET_SEALS);
    int has_seal = (seals & F_SEAL_WRITE) != 0;
    close(fd);
    printf("memfd_seal created=%d seal=%d denied=%d has_seal=%d\n", created, seal, denied, has_seal);
    return 0;
}

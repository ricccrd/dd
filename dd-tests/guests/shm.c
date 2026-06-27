// SysV shared memory round-trip: create a segment, attach + write, detach, re-attach + read it back.
// Exercises shmget/shmat/shmdt/shmctl through the JIT (with per-container key namespacing).
#include <stdio.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/shm.h>

int main(void) {
    int id = shmget(IPC_PRIVATE, 4096, IPC_CREAT | 0666);
    if (id < 0) { perror("shmget"); return 1; }
    char *p = (char *)shmat(id, NULL, 0);
    if (p == (char *)-1) { perror("shmat"); return 1; }
    strcpy(p, "SHM-ROUNDTRIP-OK");
    shmdt(p);
    char *q = (char *)shmat(id, NULL, 0); // a fresh attach should see the same bytes
    if (q == (char *)-1) { perror("shmat2"); return 1; }
    printf("%s\n", q);
    shmdt(q);
    shmctl(id, IPC_RMID, NULL);
    return 0;
}

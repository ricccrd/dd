// System V shared memory in depth: shmget + IPC_STAT (size check) + a read/write attach and a second
// SHM_RDONLY attach reading the same data, then RMID. Verifies the full shm lifecycle. Portable, golden.
#include <stdio.h>
#include <sys/ipc.h>
#include <sys/shm.h>
int main(void) {
    int id = shmget(IPC_PRIVATE, 8192, IPC_CREAT | 0600);
    if (id < 0) { perror("shmget"); return 1; }
    struct shmid_ds ds; shmctl(id, IPC_STAT, &ds);
    int size_ok = ds.shm_segsz >= 8192;
    int *a = shmat(id, 0, 0);
    for (int i = 0; i < 1024; i++) a[i] = i;
    long sum = 0; for (int i = 0; i < 1024; i++) sum += a[i];
    int *b = shmat(id, 0, SHM_RDONLY);
    long sum2 = 0; for (int i = 0; i < 1024; i++) sum2 += b[i];
    shmdt(a); shmdt(b); shmctl(id, IPC_RMID, 0);
    printf("sysv_shm size_ok=%d sum=%ld sum2=%ld\n", size_ok, sum, sum2); // 1 523776 523776
    return 0;
}

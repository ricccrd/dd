// System V semaphore set (3 sems): SETALL initial values, a 3-op semop batch, then GETVAL/GETALL to
// confirm each counter's new value. Verifies multi-sem ops + control commands. Portable, golden.
#include <stdio.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#if !defined(__APPLE__)
union semun { int val; struct semid_ds *buf; unsigned short *array; };
#endif
int main(void) {
    int id = semget(IPC_PRIVATE, 3, IPC_CREAT | 0600);
    if (id < 0) { perror("semget"); return 1; }
    union semun su; unsigned short vals[3] = {5, 10, 15}; su.array = vals;
    semctl(id, 0, SETALL, su);
    struct sembuf ops[3] = {{0, -2, 0}, {1, 3, 0}, {2, -5, 0}};
    semop(id, ops, 3);
    int v0 = semctl(id, 0, GETVAL), v1 = semctl(id, 1, GETVAL), v2 = semctl(id, 2, GETVAL);
    unsigned short out[3]; union semun rd; rd.array = out; semctl(id, 0, GETALL, rd);
    semctl(id, 0, IPC_RMID, su);
    printf("sysv_sem v0=%d v1=%d v2=%d all=%d,%d,%d\n", v0, v1, v2, out[0], out[1], out[2]); // 3 13 10
    return 0;
}

// SysV semaphore: init to 1, wait (->0), post (->1), reading the value each step. Exercises
// semget/semop/semctl(SETVAL/GETVAL/IPC_RMID) through the JIT (with the Linux->macOS cmd translation).
#include <stdio.h>
#include <sys/ipc.h>
#include <sys/sem.h>

union semun { int val; struct semid_ds *buf; unsigned short *array; };

int main(void) {
    int id = semget(IPC_PRIVATE, 1, IPC_CREAT | 0666);
    if (id < 0) { perror("semget"); return 1; }
    union semun a;
    a.val = 1;
    if (semctl(id, 0, SETVAL, a) < 0) { perror("setval"); return 1; }
    struct sembuf wait = {0, -1, 0}, post = {0, 1, 0};
    semop(id, &wait, 1);                 // 1 -> 0
    int v = semctl(id, 0, GETVAL);
    semop(id, &post, 1);                 // 0 -> 1
    int w = semctl(id, 0, GETVAL);
    printf("SEM v=%d w=%d\n", v, w);     // expect v=0 w=1
    semctl(id, 0, IPC_RMID);
    return 0;
}

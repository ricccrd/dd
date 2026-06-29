// System V IPC: a private shared-memory segment (shmget/shmat) plus a semaphore (semget/semop)
// used as a handshake between parent and child. The child fills the segment, posts the semaphore,
// the parent waits on it then reads. Exercises the SysV shm + sem syscalls. Portable -> all
// engines, golden-checked.
#include <stdio.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <sys/wait.h>
#include <unistd.h>

#if !defined(__APPLE__)
// glibc leaves `union semun` for the caller to define; macOS already declares it in <sys/sem.h>.
union semun {
    int val;
    struct semid_ds *buf;
    unsigned short *array;
};
#endif

int main(void) {
    int shmid = shmget(IPC_PRIVATE, 4096, IPC_CREAT | 0600);
    if (shmid < 0) { perror("shmget"); return 1; }
    int semid = semget(IPC_PRIVATE, 1, IPC_CREAT | 0600);
    if (semid < 0) { perror("semget"); return 1; }
    union semun su;
    su.val = 0;
    semctl(semid, 0, SETVAL, su);

    int *seg = shmat(shmid, 0, 0);
    if (seg == (void *)-1) { perror("shmat"); return 1; }

    pid_t pid = fork();
    if (pid == 0) {
        int *s = shmat(shmid, 0, 0);
        long t = 0;
        for (int i = 0; i < 512; i++) { s[i] = i + 1; t += i + 1; }
        s[512] = (int)t;
        struct sembuf up = {0, 1, 0}; // post
        semop(semid, &up, 1);
        shmdt(s);
        _exit(0);
    }
    struct sembuf down = {0, -1, 0}; // wait
    semop(semid, &down, 1);
    long sum = seg[512];
    shmdt(seg);
    shmctl(shmid, IPC_RMID, 0);
    semctl(semid, 0, IPC_RMID, su);
    waitpid(pid, NULL, 0);
    printf("sysvshm sum=%ld\n", sum); // 131328
    return 0;
}

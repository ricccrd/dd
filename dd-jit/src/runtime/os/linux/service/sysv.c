// Extracted from service(): SysV IPC syscalls (shm/sem/msg). Returns 1 if nr was handled (G_RET set), 0 otherwise.
// Included by service.c after service/helpers.c, before service(); sees the same TU scope (globals + helpers).
static int svc_sysv(struct cpu *c, uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    switch (nr) {
    // ===================== SysV shared memory (per-container key namespace) =====================
    case 194: { // shmget(key, size, shmflg)
        int r = shmget(ipc_ns_key((key_t)a0), (size_t)a1, (int)a2);
        G_RET(c) = r < 0 ? (uint64_t)(-errno) : (uint64_t)r;
        break;
    }
    case 196: { // shmat(shmid, shmaddr, shmflg) -- the guest runs in-process so the host map is usable
        void *p = shmat((int)a0, (const void *)a1, (int)a2);
        G_RET(c) = (p == (void *)-1) ? (uint64_t)(-errno) : (uint64_t)p;
        break;
    }
    case 197: { // shmdt(shmaddr)
        int r = shmdt((const void *)a0);
        G_RET(c) = r < 0 ? (uint64_t)(-errno) : 0;
        break;
    }
    case 195: { // shmctl(shmid, cmd, buf): IPC_RMID supported; STAT/SET deferred (macOS struct differs)
        if ((int)a1 == IPC_RMID) {
            int r = shmctl((int)a0, IPC_RMID, NULL);
            G_RET(c) = r < 0 ? (uint64_t)(-errno) : 0;
        } else {
            G_RET(c) = (uint64_t)(-ENOSYS);
        }
        break;
    }

    // ===================== SysV semaphores =====================
    case 190: { // semget(key, nsems, semflg)
        int r = semget(ipc_ns_key((key_t)a0), (int)a1, (int)a2);
        G_RET(c) = r < 0 ? (uint64_t)(-errno) : (uint64_t)r;
        break;
    }
    case 192: // semtimedop -> semop (glibc routes semop() through it; macOS has no timed variant)
    case 193: { // semop(semid, sops, nsops) -- struct sembuf is layout-compatible with the guest's
        int r = semop((int)a0, (struct sembuf *)a1, (size_t)a2);
        G_RET(c) = r < 0 ? (uint64_t)(-errno) : 0;
        break;
    }
    case 191: { // semctl(semid, semnum, cmd, arg)
        int lc = (int)a2, mc = sem_cmd_l2m(lc), r;
        union semun_ { int val; struct semid_ds *buf; unsigned short *array; } arg;
        if (lc == 16) { arg.val = (int)a3; r = semctl((int)a0, (int)a1, mc, arg); }                       // SETVAL
        else if (lc == 13 || lc == 17) { arg.array = (unsigned short *)a3; r = semctl((int)a0, (int)a1, mc, arg); } // GET/SETALL
        else if (lc == 0 || lc == 11 || lc == 12 || lc == 14 || lc == 15) { r = semctl((int)a0, (int)a1, mc); }    // RMID/GETPID/GETVAL/GETNCNT/GETZCNT
        else { G_RET(c) = (uint64_t)(-ENOSYS); break; }                                                   // IPC_STAT/SET: struct differs
        G_RET(c) = r < 0 ? (uint64_t)(-errno) : (uint64_t)r;
        break;
    }

    // ===================== SysV message queues =====================
    case 186: { // msgget(key, msgflg)
        int r = msgget(ipc_ns_key((key_t)a0), (int)a1);
        G_RET(c) = r < 0 ? (uint64_t)(-errno) : (uint64_t)r;
        break;
    }
    case 189: { // msgsnd(msqid, msgp, msgsz, msgflg) -- msgbuf {long mtype; char mtext[]} is compatible
        int r = msgsnd((int)a0, (const void *)a1, (size_t)a2, (int)a3);
        G_RET(c) = r < 0 ? (uint64_t)(-errno) : 0;
        break;
    }
    case 188: { // msgrcv(msqid, msgp, msgsz, msgtyp, msgflg)
        ssize_t r = msgrcv((int)a0, (void *)a1, (size_t)a2, (long)a3, (int)a4);
        G_RET(c) = r < 0 ? (uint64_t)(-errno) : (uint64_t)r;
        break;
    }
    case 187: { // msgctl(msqid, cmd, buf): IPC_RMID supported; STAT/SET deferred (macOS struct differs)
        if ((int)a1 == IPC_RMID) {
            int r = msgctl((int)a0, IPC_RMID, NULL);
            G_RET(c) = r < 0 ? (uint64_t)(-errno) : 0;
        } else {
            G_RET(c) = (uint64_t)(-ENOSYS);
        }
        break;
    }
    default: return 0;
    }
    return 1;
}

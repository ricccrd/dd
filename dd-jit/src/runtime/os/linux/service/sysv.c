// Extracted from service(): SysV IPC syscalls (shm/sem/msg). Returns 1 if nr was handled (G_RET set), 0 otherwise.
// Included by service.c after service/helpers.c, before service(); sees the same TU scope (globals + helpers).

// The guest's `struct shmid64_ds` (aarch64 asm-generic, 64-bit) as it expects IPC_STAT to fill it. We
// translate the macOS host `struct shmid_ds` into this layout by field name; the offsets below match the
// guest ABI (ipc64_perm = 48 bytes, shmid64_ds = 112 bytes).
struct ipc64_perm_guest {
    int32_t key;
    uint32_t uid, gid, cuid, cgid;
    uint32_t mode;
    uint16_t seq, pad2;
    uint64_t unused1, unused2;
};
struct shmid64_ds_guest {
    struct ipc64_perm_guest shm_perm;
    uint64_t shm_segsz;
    int64_t shm_atime, shm_dtime, shm_ctime;
    int32_t shm_cpid, shm_lpid;
    uint64_t shm_nattch, unused4, unused5;
};

// macOS shmget rounds the segment size up to a page, and its IPC_STAT reports that rounded size; Linux
// reports the size the caller originally requested. Remember each segment's requested size (keyed by the
// host shmid) so IPC_STAT can report it faithfully -- otherwise the guest's `shm_segsz >= requested`
// check can fail.
#define SHM_SEGSZ_MAX 256
static struct {
    int used, id;
    size_t segsz;
} g_shm_segsz[SHM_SEGSZ_MAX];
static pthread_mutex_t g_shm_segsz_m = PTHREAD_MUTEX_INITIALIZER;

static void shm_segsz_remember(int id, size_t segsz) {
    pthread_mutex_lock(&g_shm_segsz_m);
    int slot = -1;
    for (int i = 0; i < SHM_SEGSZ_MAX; i++) {
        if (g_shm_segsz[i].used && g_shm_segsz[i].id == id) {
            slot = i;
            break;
        }
        if (slot < 0 && !g_shm_segsz[i].used) slot = i;
    }
    if (slot >= 0) {
        g_shm_segsz[slot].used = 1;
        g_shm_segsz[slot].id = id;
        g_shm_segsz[slot].segsz = segsz;
    }
    pthread_mutex_unlock(&g_shm_segsz_m);
}

static size_t shm_segsz_lookup(int id) {
    size_t r = 0;
    pthread_mutex_lock(&g_shm_segsz_m);
    for (int i = 0; i < SHM_SEGSZ_MAX; i++)
        if (g_shm_segsz[i].used && g_shm_segsz[i].id == id) {
            r = g_shm_segsz[i].segsz;
            break;
        }
    pthread_mutex_unlock(&g_shm_segsz_m);
    return r;
}

static void shm_segsz_forget(int id) {
    pthread_mutex_lock(&g_shm_segsz_m);
    for (int i = 0; i < SHM_SEGSZ_MAX; i++)
        if (g_shm_segsz[i].used && g_shm_segsz[i].id == id) {
            g_shm_segsz[i].used = 0;
            break;
        }
    pthread_mutex_unlock(&g_shm_segsz_m);
}

// shmctl(IPC_STAT): query the host, then marshal its `struct shmid_ds` into the guest aarch64 layout at
// `gbuf`. Reports the guest-requested segment size when we have it. Returns the G_RET value to set.
static uint64_t shm_stat_to_guest(int id, uint64_t gbuf) {
    struct shmid_ds h;
    if (shmctl(id, IPC_STAT, &h) < 0) return (uint64_t)(-errno);
    struct shmid64_ds_guest *g = (struct shmid64_ds_guest *)gbuf;
    memset(g, 0, sizeof *g);
    g->shm_perm.key = h.shm_perm._key;
    g->shm_perm.uid = h.shm_perm.uid;
    g->shm_perm.gid = h.shm_perm.gid;
    g->shm_perm.cuid = h.shm_perm.cuid;
    g->shm_perm.cgid = h.shm_perm.cgid;
    g->shm_perm.mode = h.shm_perm.mode;
    g->shm_perm.seq = h.shm_perm._seq;
    size_t req = shm_segsz_lookup(id);
    g->shm_segsz = req ? req : (uint64_t)h.shm_segsz;
    g->shm_atime = h.shm_atime;
    g->shm_dtime = h.shm_dtime;
    g->shm_ctime = h.shm_ctime;
    g->shm_cpid = h.shm_cpid;
    g->shm_lpid = h.shm_lpid;
    g->shm_nattch = (uint64_t)h.shm_nattch;
    return 0;
}

static int svc_sysv(struct cpu *c, uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                    uint64_t a5) {
    switch (nr) {
    // ===================== SysV shared memory (per-container key namespace) =====================
    case 194: { // shmget(key, size, shmflg)
        int r = shmget(ipc_ns_key((key_t)a0), (size_t)a1, (int)a2);
        if (r >= 0 && a1)
            shm_segsz_remember(r, (size_t)a1); // remember requested size for IPC_STAT (skip size-0 lookups)
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
    case 195: { // shmctl(shmid, cmd, buf): IPC_RMID + IPC_STAT supported; IPC_SET deferred (macOS struct differs)
        if ((int)a1 == IPC_RMID) {
            int r = shmctl((int)a0, IPC_RMID, NULL);
            if (r == 0) shm_segsz_forget((int)a0);
            G_RET(c) = r < 0 ? (uint64_t)(-errno) : 0;
        } else if ((int)a1 == IPC_STAT) {
            G_RET(c) = shm_stat_to_guest((int)a0, a2);
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
    case 192:   // semtimedop -> semop (glibc routes semop() through it; macOS has no timed variant)
    case 193: { // semop(semid, sops, nsops) -- struct sembuf is layout-compatible with the guest's
        int r = semop((int)a0, (struct sembuf *)a1, (size_t)a2);
        G_RET(c) = r < 0 ? (uint64_t)(-errno) : 0;
        break;
    }
    case 191: { // semctl(semid, semnum, cmd, arg)
        int lc = (int)a2, mc = sem_cmd_l2m(lc), r;
        union semun_ {
            int val;
            struct semid_ds *buf;
            unsigned short *array;
        } arg;
        if (lc == 16) {
            arg.val = (int)a3;
            r = semctl((int)a0, (int)a1, mc, arg);
        } // SETVAL
        else if (lc == 13 || lc == 17) {
            arg.array = (unsigned short *)a3;
            r = semctl((int)a0, (int)a1, mc, arg);
        } // GET/SETALL
        else if (lc == 0 || lc == 11 || lc == 12 || lc == 14 || lc == 15) {
            r = semctl((int)a0, (int)a1, mc);
        } // RMID/GETPID/GETVAL/GETNCNT/GETZCNT
        else {
            G_RET(c) = (uint64_t)(-ENOSYS);
            break;
        } // IPC_STAT/SET: struct differs
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

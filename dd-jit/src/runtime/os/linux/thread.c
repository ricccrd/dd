// dd/runtime/os/linux -- threads & futex (clone -> pthread; per-thread cpu; futex via condvars).

// ---------------- syscalls ----------------
// ---------------- threads & futex ----------------
// fwd: thread trampoline runs the dispatcher
static void run_guest(struct cpu *c);

// One global wait queue. Coarse but correct: a waker takes the lock, so it can't
// slip between a waiter's value-check and its wait. Waiters re-check their own
// address, so a broadcast is sound (just not maximally efficient).
static pthread_mutex_t g_futex_m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_futex_c = PTHREAD_COND_INITIALIZER;
static long futex_op(int *uaddr, int op, int val, const struct timespec *ts) {
    // FUTEX_WAIT / WAIT_BITSET: sleep while *uaddr == val
    if (op == 0 || op == 9) {
        pthread_mutex_lock(&g_futex_m);
        if (__atomic_load_n(uaddr, __ATOMIC_SEQ_CST) != val) {
            pthread_mutex_unlock(&g_futex_m);
            return -EAGAIN;
        // EAGAIN
        }
        if (ts) {
            struct timespec abs;
            clock_gettime(CLOCK_REALTIME, &abs);
            abs.tv_sec += ts->tv_sec;
            abs.tv_nsec += ts->tv_nsec;
            if (abs.tv_nsec >= 1000000000) {
                abs.tv_sec++;
                abs.tv_nsec -= 1000000000;
            }
            pthread_cond_timedwait(&g_futex_c, &g_futex_m, &abs);
        } else
            pthread_cond_wait(&g_futex_c, &g_futex_m);
        pthread_mutex_unlock(&g_futex_m);
        return 0;
    }
    // FUTEX_WAKE / WAKE_BITSET: wake up to `val` waiters
    if (op == 1 || op == 10) {
        pthread_mutex_lock(&g_futex_m);
        // they re-check their own addr
        pthread_cond_broadcast(&g_futex_c);
        pthread_mutex_unlock(&g_futex_m);
        return val;
    }
    // other ops: pretend success
    return 0;
}
static void futex_wake_addr(uint64_t uaddr) {
    if (!uaddr) return;
    // CLONE_CHILD_CLEARTID: zero then wake joiners
    *(int *)uaddr = 0;
    pthread_mutex_lock(&g_futex_m);
    pthread_cond_broadcast(&g_futex_c);
    pthread_mutex_unlock(&g_futex_m);
}

static volatile int g_next_tid = 1000;
static void *thread_trampoline(void *p) {
    struct cpu *child = (struct cpu *)p;
    // sets its own TSD, runs to thread exit
    run_guest(child);
    // cgroup pids: task ended
    atomic_fetch_sub(&g_pids_cur, 1);
    // pthread_join waits on this
    futex_wake_addr(child->ctid);
    free(child);
    return NULL;
}
// Spawn a guest thread sharing this address space. stack_top is the initial sp.
static int spawn_thread(struct cpu *parent, uint64_t flags, uint64_t stack_top, uint64_t tls, uint64_t ptid,
                        uint64_t ctid) {
    // cgroup pids.max
    if (g_pids_max && atomic_load(&g_pids_cur) >= g_pids_max) return -EAGAIN;
    struct cpu *child = malloc(sizeof *child);
    // ENOMEM
    if (!child) return -12;
    *child = *parent;
    // child sees clone return 0
    G_RET(child) = 0;
    G_SP(child) = stack_top;
    // resume just after the clone svc
    G_THREAD_RESUME(child, parent);
    // §B: child starts with an EMPTY shadow stack (no parent frames)
    G_SHADOW_RESET(child);
    child->exited = 0;
    child->redirect = 0;
    // CLONE_SETTLS
    if (flags & 0x00080000) G_TLS(child) = tls;
    int tid = __sync_add_and_fetch(&g_next_tid, 1);
    // CLONE_PARENT_SETTID
    if ((flags & 0x00100000) && ptid) *(int *)ptid = tid;
    // CLONE_CHILD_SETTID
    if ((flags & 0x01000000) && ctid) *(int *)ctid = tid;
    // CLONE_CHILD_CLEARTID
    child->ctid = (flags & 0x00200000) ? ctid : 0;
    g_threaded = 1;
    pthread_t th;
    if (pthread_create(&th, NULL, thread_trampoline, child) != 0) {
        free(child);
        return -EAGAIN;
    }
    // cgroup pids: task created
    atomic_fetch_add(&g_pids_cur, 1);
    pthread_detach(th);
    return tid;
}

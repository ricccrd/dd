// dd/runtime/os/linux -- threads & futex (clone -> pthread; per-thread cpu; futex via condvars).

// ---------------- syscalls ----------------
// ---------------- threads & futex ----------------
// fwd: thread trampoline runs the dispatcher
static void run_guest(struct cpu *c);

// ---------------- futex: per-address hashed wait queues ----------------
// Legacy (NOFUTEXQ=1): ONE global mutex + condvar. Correct but a WAKE on ANY address takes
// the global lock and broadcasts EVERY waiter on EVERY address (thundering herd) -> the real
// multi-thread DB bottleneck. The S3 uncontended fast path helped only the no-sleeper case.
//
// W5C (default): a fixed table of per-address buckets {mutex, condvar, waiter-count}, keyed by
// hash(uaddr). A WAKE touches only the bucket for that address; uncontended/no-sleeper WAKE is a
// single SEQ_CST counter load with NO lock. Addresses that collide in a bucket share its lock
// (occasional extra spurious wakeups, never a missed wakeup). Correctness:
//   * Lost-wakeup closed by a Dekker/SEQ_CST handshake on the no-sleeper fast path: the WAITER
//     increments bucket.waiters (SEQ_CST) BEFORE loading *uaddr (SEQ_CST); the WAKER stores *uaddr
//     (the guest did this before the syscall; we add a SEQ_CST fence) BEFORE loading bucket.waiters
//     (SEQ_CST). By the single total order of SEQ_CST ops, if the waker reads waiters==0 (and so
//     skips the wake), the waiter is guaranteed to read the new *uaddr and bail with EAGAIN rather
//     than sleep. If the waker reads waiters>=1 it takes the lock+broadcast and the waiter (still
//     under the bucket mutex) is reliably signalled.
//   * A real blocked waiter is always woken because waker and waiter use the SAME bucket mutex for
//     that uaddr, so the waker cannot slip between the waiter's value-check and its cond_wait.
//   * FUTEX_WAIT may return 0 spuriously (per spec); the guest re-checks the word and re-waits.
#define FUTEX_NBUCKET 256
struct futex_bucket {
    pthread_mutex_t m;
    pthread_cond_t c;
    _Atomic int waiters;
};
// _xproc-futex-fork_: the bucket table lives in a MAP_SHARED anonymous region whose mutex/condvar are
// PTHREAD_PROCESS_SHARED, so a FUTEX_WAKE in one process matches a FUTEX_WAIT in another across dd's
// fork() -- e.g. a glibc process-shared (named/unnamed-on-shm) semaphore where the child sem_post()s
// and the parent sem_wait()s. dd's fork() is a real host fork(): the child inherits the identical guest
// address space, so a shared-memory futex word resolves to the SAME host address in parent and child
// and both hash to the same bucket, while the underlying MAP_SHARED guest page is one physical page.
// The table is created ONCE at engine startup (constructor, before any guest fork) so every forked
// worker inherits the same physical buckets. The lock-free no-sleeper WAKE fast path is unchanged --
// only the slow path (a real sleeper exists) touches the now-cross-process mutex/condvar. In-process
// (multi-threaded) futexes still hit the same table, keyed by their shared virtual address, as before.
static struct futex_bucket *g_fbk;
static void futex_table_init(void) {
    if (g_fbk) return;
    size_t sz = sizeof(struct futex_bucket) * FUTEX_NBUCKET;
    void *mem = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
    if (mem == MAP_FAILED) // cross-process wakeups degrade, but in-process futexes still work
        mem = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (mem == MAP_FAILED) abort();
    struct futex_bucket *t = (struct futex_bucket *)mem;
    pthread_mutexattr_t ma;
    pthread_condattr_t ca;
    pthread_mutexattr_init(&ma);
    pthread_condattr_init(&ca);
    pthread_mutexattr_setpshared(&ma, PTHREAD_PROCESS_SHARED);
    pthread_condattr_setpshared(&ca, PTHREAD_PROCESS_SHARED);
    for (int i = 0; i < FUTEX_NBUCKET; i++) {
        pthread_mutex_init(&t[i].m, &ma);
        pthread_cond_init(&t[i].c, &ca);
        atomic_store_explicit(&t[i].waiters, 0, memory_order_relaxed);
    }
    pthread_mutexattr_destroy(&ma);
    pthread_condattr_destroy(&ca);
    g_fbk = t;
}
__attribute__((constructor)) static void futex_table_ctor(void) { futex_table_init(); }
static inline struct futex_bucket *fbk_of(const void *uaddr) {
    uint32_t h = (uint32_t)(((uintptr_t)uaddr >> 2) * 2654435761u) & (FUTEX_NBUCKET - 1);
    return &g_fbk[h];
}
// legacy global queue (NOFUTEXQ=1)
static pthread_mutex_t g_futex_m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_futex_c = PTHREAD_COND_INITIALIZER;
// PROF: fast (no-lock) wakes, slow (locked) wakes, eagain pre-checks
static uint64_t g_futex_wake_fast, g_futex_wake_slow, g_futex_wait_n;

static void abs_from_rel(struct timespec *abs, const struct timespec *ts) {
    clock_gettime(CLOCK_REALTIME, abs);
    abs->tv_sec += ts->tv_sec;
    abs->tv_nsec += ts->tv_nsec;
    if (abs->tv_nsec >= 1000000000) {
        abs->tv_sec++;
        abs->tv_nsec -= 1000000000;
    }
}
// FUTEX_WAIT_BITSET (op 9) passes an ABSOLUTE deadline, not a relative duration: against
// CLOCK_REALTIME when FUTEX_CLOCK_REALTIME is set (e.g. glibc's pthread_cond_timedwait on a
// CLOCK_REALTIME condvar) and CLOCK_MONOTONIC otherwise. That clock flag is masked off before
// the syscall reaches us, so we recover the intended clock from the deadline itself: only one
// of the two clocks leaves a sane remaining time -- the other is off by the decades-wide gap
// between realtime (~now since 1970) and monotonic (~uptime), yielding a negative or absurdly
// large value. Fills `rel` with the remaining time until the deadline, clamped at zero.
static void futex_rel_from_abs(struct timespec *rel, const struct timespec *deadline) {
    struct timespec rt, mono;
    clock_gettime(CLOCK_REALTIME, &rt);
    clock_gettime(CLOCK_MONOTONIC, &mono);
    int64_t drt = (int64_t)(deadline->tv_sec - rt.tv_sec) * 1000000000 + (deadline->tv_nsec - rt.tv_nsec);
    int64_t dmono = (int64_t)(deadline->tv_sec - mono.tv_sec) * 1000000000 + (deadline->tv_nsec - mono.tv_nsec);
    int64_t ns;
    if (drt < 0)
        ns = dmono; // deadline predates realtime "now" -> it must be a monotonic deadline
    else if (dmono < 0)
        ns = drt;
    else
        ns = drt < dmono ? drt : dmono; // both plausible: the true clock gives the smaller remainder
    if (ns < 0) ns = 0;
    rel->tv_sec = ns / 1000000000;
    rel->tv_nsec = ns % 1000000000;
}
static long futex_op(int *uaddr, int op, int val, const struct timespec *ts) {
    if (!g_futexq) {
        // ---- legacy single global queue ----
        if (op == 0 || op == 9) {
            pthread_mutex_lock(&g_futex_m);
            if (__atomic_load_n(uaddr, __ATOMIC_SEQ_CST) != val) {
                pthread_mutex_unlock(&g_futex_m);
                return -EAGAIN;
            }
            int rc = 0;
            if (ts) {
                struct timespec abs, rel;
                // op 9 (FUTEX_WAIT_BITSET): ts is an absolute deadline; op 0: it is relative.
                if (op == 9) futex_rel_from_abs(&rel, ts);
                abs_from_rel(&abs, op == 9 ? &rel : ts);
                rc = pthread_cond_timedwait(&g_futex_c, &g_futex_m, &abs);
            } else
                pthread_cond_wait(&g_futex_c, &g_futex_m);
            pthread_mutex_unlock(&g_futex_m);
            return rc == ETIMEDOUT ? -ETIMEDOUT : 0;
        }
        if (op == 1 || op == 10) {
            pthread_mutex_lock(&g_futex_m);
            pthread_cond_broadcast(&g_futex_c);
            pthread_mutex_unlock(&g_futex_m);
            return val;
        }
        return 0;
    }
    // ---- W5C per-address buckets ----
    struct futex_bucket *b = fbk_of(uaddr);
    // FUTEX_WAIT / WAIT_BITSET: sleep while *uaddr == val
    if (op == 0 || op == 9) {
        if (g_prof) g_futex_wait_n++;
        pthread_mutex_lock(&b->m);
        // Publish "a waiter is arriving on this bucket" BEFORE checking the word, both SEQ_CST,
        // so a concurrent lock-free WAKE either sees waiters>=1 (and locks+wakes us) or we see
        // the waker's new *uaddr here and bail -> no lost wakeup. (Counter held up across the
        // value-check so the no-sleeper waker can't observe a gap.)
        atomic_fetch_add_explicit(&b->waiters, 1, memory_order_seq_cst);
        if (__atomic_load_n(uaddr, __ATOMIC_SEQ_CST) != val) {
            atomic_fetch_sub_explicit(&b->waiters, 1, memory_order_seq_cst);
            pthread_mutex_unlock(&b->m);
            return -EAGAIN;
        }
        int rc = 0;
        if (ts) {
            struct timespec abs, rel;
            // op 9 (FUTEX_WAIT_BITSET): ts is an absolute deadline; op 0: it is relative.
            if (op == 9) futex_rel_from_abs(&rel, ts);
            abs_from_rel(&abs, op == 9 ? &rel : ts);
            rc = pthread_cond_timedwait(&b->c, &b->m, &abs);
        } else
            pthread_cond_wait(&b->c, &b->m);
        atomic_fetch_sub_explicit(&b->waiters, 1, memory_order_seq_cst);
        pthread_mutex_unlock(&b->m);
        // A pure-timeout wait must report -ETIMEDOUT so the guest stops re-waiting.
        return rc == ETIMEDOUT ? -ETIMEDOUT : 0;
    }
    // FUTEX_WAKE / WAKE_BITSET: wake up to `val` waiters on THIS address's bucket only
    if (op == 1 || op == 10) {
        // The guest changed *uaddr before this syscall; fence so that store is ordered before the
        // waiters load (SEQ_CST), completing the Dekker handshake with an arriving waiter.
        atomic_thread_fence(memory_order_seq_cst);
        if (atomic_load_explicit(&b->waiters, memory_order_seq_cst) == 0) {
            if (g_prof) g_futex_wake_fast++;
            return val; // no sleeper in this bucket -> no lock, no broadcast
        }
        if (g_prof) g_futex_wake_slow++;
        pthread_mutex_lock(&b->m);
        pthread_cond_broadcast(&b->c); // waiters re-check their own word; spurious wakes are legal
        pthread_mutex_unlock(&b->m);
        return val;
    }
    // other ops (REQUEUE/CMP_REQUEUE/WAKE_OP/...): unchanged -- pretend success (baseline behavior)
    return 0;
}
static void futex_wake_addr(uint64_t uaddr) {
    if (!uaddr) return;
    // CLONE_CHILD_CLEARTID: zero then wake joiners (pthread_join FUTEX_WAITs on this word)
    *(int *)uaddr = 0;
    if (!g_futexq) {
        pthread_mutex_lock(&g_futex_m);
        pthread_cond_broadcast(&g_futex_c);
        pthread_mutex_unlock(&g_futex_m);
        return;
    }
    struct futex_bucket *b = fbk_of((const void *)(uintptr_t)uaddr);
    atomic_thread_fence(memory_order_seq_cst);
    if (atomic_load_explicit(&b->waiters, memory_order_seq_cst) == 0) return;
    pthread_mutex_lock(&b->m);
    pthread_cond_broadcast(&b->c);
    pthread_mutex_unlock(&b->m);
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

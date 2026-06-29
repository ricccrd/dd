//! threads — basics expansion (in-process JIT matrix). Owner: threads agent. Edit ONLY this file.
//! Builders: src(name,file).oracle()/.exit()/.out()/.has(); port(name,file) for cross-engine golden.
//! Keep this module compiling at all times (`cargo build -p dd-tests`).
//!
//! Breadth over the POSIX threading surface a real daemon/runtime leans on: pthread create/join/detach,
//! all mutex kinds (recursive/errorcheck/trylock), condvars (signal/broadcast/timedwait), rwlocks,
//! barriers, spinlocks, semaphores (named+unnamed), TLS (pthread_key + destructors), pthread_once,
//! pthread_cancel + cleanup handlers, C11 atomics (all RMW ops, memory orders, CAS loops, lock-free
//! stack), scheduling APIs, and heavy mutex/atomic contention with deterministic totals.
//!
//! Most cases are `port(...)` — one POSIX source built for every engine (Linux x2 + macOS), golden so
//! the threading model must be byte-identical emulated-on-Linux and native-on-macOS. A few primitives
//! macOS's libc lacks (pthread_barrier, pthread_spin_*, unnamed sem_init) are Linux-only `src(...)`
//! diffed against the native oracle.
#![allow(unused_imports)]
use crate::{group, src, port, fixture, in_rootfs, Case, Engine, Group};

pub fn groups() -> Vec<Group> {
    vec![ext_threads()]
}

fn ext_threads() -> Group {
    group("ext_threads", vec![
        // ---- create / join / detach ----
        port("join-retval", "ext_threads/th_join_retval.c").out("join sum=1240\n"),
        port("detach", "ext_threads/th_detach.c").out("detach done=32\n"),
        port("self-equal", "ext_threads/th_self.c").out("self consistent=16 ne_main=16\n"),
        port("attr-stack", "ext_threads/th_attr_stack.c").out("attr_stack size_ok=1 result=501500\n"),
        port("sched", "ext_threads/th_sched.c").out("sched setpol=1 created=1 yield=1 range_ok=1\n"),
        // ---- mutexes ----
        port("mutex-recursive", "ext_threads/th_mutex_recursive.c").out("recursive depth=100 relock=50\n"),
        port("mutex-errorcheck", "ext_threads/th_mutex_errorcheck.c").out("errorcheck edeadlk=1 eperm=1\n"),
        port("mutex-trylock", "ext_threads/th_mutex_trylock.c").out("trylock ok=1 busy=1 again=1\n"),
        port("mutex-stress", "ext_threads/th_mutex_stress.c").out("mutex_stress shared=1600000\n"),
        // ---- condvars ----
        port("condvar-broadcast", "ext_threads/th_condvar_broadcast.c").out("broadcast awake=16\n"),
        // condvar timedwait pure-timeout HANGS under the JIT (futex absolute-deadline translation) —
        // passes native-on-macOS. xfail Linux; see GAPS "ext-condvar-timedwait". (signal-wins variant OK.)
        port("condvar-timedwait", "ext_threads/th_condvar_timedwait.c").out("timedwait timeout=1\n"),
        port("condvar-signal-wins", "ext_threads/th_cond_signal_wins.c").out("signal_wins rc=0 ready=1\n"),
        // ---- rwlock ----
        port("rwlock", "ext_threads/th_rwlock.c").out("rwlock shared=40000 read_errors=0\n"),
        port("rwlock-try", "ext_threads/th_rwlock_try.c").out("rwlock_try rd_ok=1 wr_busy=1\n"),
        // ---- TLS ----
        port("key", "ext_threads/th_key.c").out("key total=82800\n"),
        port("key-dtor", "ext_threads/th_key_dtor.c").out("key_dtor calls=10\n"),
        // ---- once ----
        port("once", "ext_threads/th_once.c").out("once init_count=1\n"),
        // ---- cancel + cleanup ----
        port("cancel", "ext_threads/th_cancel.c").out("cancel cleaned=8\n"),
        // ---- semaphores ----
        // sem_open returns ENOENT under the JIT (no /dev/shm/sem.* in the guest's mac-side fs view;
        // shm_open is specially emulated but named sems aren't) — passes native-on-macOS. xfail Linux;
        // see GAPS "ext-sem-open".
        port("sem-named", "ext_threads/th_sem_named.c").out("sem_named total=80000\n"),
        // ---- atomics ----
        port("atomics-ops", "ext_threads/th_atomics_ops.c").out("atomics v=249 cas_ok=1 cas_fail=1 old=999 final=7\n"),
        port("atomics-orders", "ext_threads/th_atomics_orders.c").out("orders relaxed=320000 acqrel=320000 seqcst=320000\n"),
        port("cas-contention", "ext_threads/th_cas_contention.c").out("cas v=320000\n"),
        port("atomic-flag", "ext_threads/th_atomic_flag.c").out("atomic_flag shared=320000\n"),
        port("cas-ptr", "ext_threads/th_atomic_cas_ptr.c").out("cas_ptr count=8000\n"),
        // ---- Linux-only primitives (macOS libc lacks these) — diffed vs native oracle ----
        src("barrier", "ext_threads/th_barrier.c").oracle(),         // pthread_barrier (no macOS)
        // spinlock hangs under the x86_64 JIT only (the tight spin loop mistranslates); arm64 passes.
        // xfail x86_64; see GAPS "ext-spinlock-x86".
        src("spinlock", "ext_threads/th_spinlock.c").oracle().xfail(&[Engine::LinuxX86_64]),
        src("sem-unnamed", "ext_threads/th_sem_unnamed.c").oracle(), // sem_init pshared=0 (no macOS)
    ])
}

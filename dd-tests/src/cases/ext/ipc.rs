//! ipc — basics expansion (in-process JIT matrix). Owner: ipc agent. Edit ONLY this file.
//! Builders: src(name,file).oracle()/.exit()/.out()/.has(); port(name,file) for cross-engine golden.
//! Keep this module compiling at all times (`cargo build -p dd-tests`).
//!
//! Breadth over inter-process communication: anonymous pipes (sum/poll/EOF/EPIPE), FIFOs (non-blocking
//! open, multi-writer, two-way request/response), System V IPC (shm with IPC_STAT + RDONLY attach, a
//! 3-semaphore set with SETALL/semop/GETALL, typed message queues + ftok keys), POSIX shared memory as
//! a cross-process atomic counter, POSIX named semaphores across a fork, SCM_RIGHTS fd passing, AF_UNIX
//! dgram framing, dup'd-fd shared offsets, and advisory file locks (flock + lockf) across a fork.
//!
//! `port(...)` cases prove the IPC behaviour is byte-identical emulated-on-Linux and native-on-macOS.
//! A few Linux-only mechanisms (POSIX mq, eventfd, SOCK_SEQPACKET) are `src(...)` diffed vs the oracle.
#![allow(unused_imports)]
use crate::{group, src, port, fixture, in_rootfs, Case, Engine, Group};

pub fn groups() -> Vec<Group> {
    vec![ext_ipc()]
}

fn ext_ipc() -> Group {
    group("ext_ipc", vec![
        // ---- pipes ----
        port("pipe", "ext_ipc/ipc_pipe.c").out("pipe sum=2001000\n"),
        port("pipe-poll", "ext_ipc/ipc_pipe_poll.c").out("pipe_poll readable=1 got=X hup=1\n"),
        port("pipe-eof", "ext_ipc/ipc_pipe_eof.c").out("pipe_eof epipe=1 first=2 eof=0\n"),
        // ---- FIFOs ----
        port("fifo-nonblock", "ext_ipc/ipc_fifo_nonblock.c").out("fifo_nb enxio=1 rd_ok=1 wr_ok=1 data=hello\n"),
        port("fifo-multi-writer", "ext_ipc/ipc_fifo_multi_writer.c").out("fifo_mw sum=1001000 got=2000\n"),
        port("fifo-twoway", "ext_ipc/ipc_fifo_twoway.c").out("fifo_twoway sum=385\n"),
        // ---- System V IPC ----
        // shmctl(IPC_STAT).shm_segsz is wrong (< requested) on the arm64 JIT only; x86_64 + macOS OK.
        // The data round-trip itself is correct. xfail arm64; see GAPS "ext-shmstat-arm".
        port("sysv-shm", "ext_ipc/ipc_sysv_shm.c").out("sysv_shm size_ok=1 sum=523776 sum2=523776\n"),
        port("sysv-sem", "ext_ipc/ipc_sysv_sem.c").out("sysv_sem v0=3 v1=13 v2=10 all=3,13,10\n"),
        port("sysv-msg", "ext_ipc/ipc_sysv_msg.c").out("sysv_msg t2=type2 any=type1 t3=type3\n"),
        port("msgget-ftok", "ext_ipc/ipc_msgget_ftok.c").out("ftok key_ok=1 msg=ftok-msg\n"),
        // ---- POSIX shm / sem ----
        port("posix-shm", "ext_ipc/ipc_posix_shm.c").out("posix_shm total=40000\n"),
        // sem_open ENOENT under the JIT (same gap as threads/sem-named) — passes native-on-macOS.
        // xfail Linux; see GAPS "ext-sem-open".
        port("posix-sem-named", "ext_ipc/ipc_posix_sem_named.c").out("posix_sem_named c=5\n"),
        // ---- fd passing / unix dgram ----
        port("scm-rights", "ext_ipc/ipc_scm_rights.c").out("scm_rights data=passed-fd-content\n"),
        port("sockpair-dgram", "ext_ipc/ipc_sockpair_dgram.c").out("sockpair_dgram lens=242\n"),
        // ---- fd offset sharing ----
        port("dup-offset", "ext_ipc/ipc_dup_offset.c").out("dup_offset a=012 b=345\n"),
        // ---- advisory locks across fork ----
        port("flock-fork", "ext_ipc/ipc_flock_fork.c").out("flock child_blocked=1 child_acquired=1\n"),
        // lockf() POSIX record-lock conflicts aren't enforced across processes under the JIT (child's
        // F_TLOCK succeeds while the parent holds the lock) — flock() above works, macOS works. xfail
        // Linux; see GAPS "ext-lockf-fork".
        port("lockf-fork", "ext_ipc/ipc_lockf.c").out("lockf blocked=1 acquired=1\n"),
        // ---- Linux-only IPC (no portable POSIX form) — diffed vs native oracle ----
        // mq_open unsupported under the JIT (no /dev/mqueue) → empty. xfail Linux; GAPS "ext-mq".
        src("mq", "ext_ipc/ipc_mq.c").oracle(),
        // eventfd counters aren't shared across fork under the JIT (child's writes don't reach the
        // parent's object → reads 0; native reads 100). xfail Linux; see GAPS "ext-eventfd-fork".
        src("eventfd", "ext_ipc/ipc_eventfd.c").oracle(),
        // socketpair(AF_UNIX, SOCK_SEQPACKET) returns -1 under the JIT → empty. xfail Linux; GAPS "ext-seqpacket".
        src("seqpacket", "ext_ipc/ipc_seqpacket.c").oracle(),
    ])
}

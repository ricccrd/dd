//! Basics EXTENSION groups — per-category expansion of the in-process JIT matrix, one file per agent so
//! many builders work without collision (mirrors src/scenarios/). The base groups stay in cases/mod.rs;
//! these add breadth. `cases::all()` appends `ext::all()`. Each file keeps itself compiling.

use crate::Group;

pub mod abi;        // codegen / ABI: int/float/simd/varargs/recursion/jumptable/fnptr/struct-abi
pub mod libc;       // string/mem/stdio/malloc/math/locale/time/regex/glob breadth
pub mod posix;      // file/dir/mmap/poll/signal/process/fs-metadata syscalls (portable + oracle)
pub mod linuxsys;   // epoll/eventfd/timerfd/signalfd/inotify/sendfile/splice/memfd/pidfd (oracle)
pub mod threads;    // mutex/condvar/rwlock/barrier/atomics/TLS/futex contention
pub mod ipc;        // pipes/fifo/sysv+posix shm/sem/msg/unix sockets/scm_rights + edge corners
pub mod net;        // tcp/udp/unix/sockopt/nonblock/sendmsg/poll-loops/half-close
pub mod soak;       // long-run JIT machinery: code-cache/IBTC/SMC/churn endurance
pub mod darwin;     // macOS-native (lighter-touch): kqueue/sysctl/mach/Mach-O ABI corners
pub mod completeness; // syscall-table + x86-64/aarch64 opcode COMPLETENESS probes (no images)

pub fn all() -> Vec<Group> {
    let mut g = vec![];
    g.extend(abi::groups());
    g.extend(libc::groups());
    g.extend(posix::groups());
    g.extend(linuxsys::groups());
    g.extend(threads::groups());
    g.extend(ipc::groups());
    g.extend(net::groups());
    g.extend(soak::groups());
    g.extend(darwin::groups());
    g.extend(completeness::groups());
    g
}

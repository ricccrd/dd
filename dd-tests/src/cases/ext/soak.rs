//! soak — basics expansion (in-process JIT matrix). Owner: soak agent. Edit ONLY this file.
//! Keep this module compiling at all times (`cargo build -p dd-tests`).
//!
//! LONG / endurance tier (heavy class). Each guest runs for a sustained stretch (tens of millions of
//! iterations, or thousands of OS-resource cycles) and is built so it can ONLY fail through the JIT's
//! long-run machinery — code-cache eviction/recycle under many distinct hot blocks, block-chaining /
//! IBTC drift over tens of millions of indirect calls, deep call-graph and recursion-depth churn,
//! self-modifying-code re-translation, sustained thread / fork / heap / mmap churn, page-fault & TLB
//! pressure, long FP accumulation, signal delivery under load, and setjmp/longjmp transfer churn. These
//! catch bugs a short test never reaches. Every case has a FIXED final accumulator/total, so it is
//! golden-checkable (portable guests, identical on Linux x2 + darwin) or oracle-checkable (the aarch64
//! SMC guest, diffed against a native run). Iteration counts are tuned to stay well under the harness's
//! 25s timeout. Golden values were computed natively and cross-checked aarch64 vs x86-64 (qemu) so the
//! `.out(...)` strings are arch-independent.
#![allow(unused_imports)]
use crate::{group, src, port, darwin_src, darwin_libc, fixture, in_rootfs, Case, Engine, Group};

pub fn groups() -> Vec<Group> { vec![soakext()] }

fn soakext() -> Group {
    group("soakext", vec![
        // --- code-cache / dispatch / indirect-branch endurance ---
        // 1024 distinct translated blocks driven in data-dependent order, ~60M iters: 4x the block
        // population of base soak/codecache -> far more code-cache eviction + re-translation mid-run.
        port("manyblocks", "ext_soak/manyblocks.c").out("soak manyblocks acc=9715475295547480376\n"),
        // megamorphic virtual dispatch: 256 objects, method pointer LOADED from the struct, ~80M calls
        // -> single call site, constantly shifting target reached via memory load (IBTC/inline-cache drift).
        port("vtable", "ext_soak/vtable.c").out("soak vtable acc=3810013136533084236\n"),

        // --- call-graph / recursion-depth churn (genuine CALL/RET, noinline) ---
        // tribonacci-style 3-way mutual recursion, ~9M nested calls over a deep, ever-changing frame stack.
        port("callgraph", "ext_soak/callgraph.c").out("soak callgraph val=569213868\n"),
        // 6000-deep non-tail recursion repeated 6000x (~36M calls) -> guest-stack growth/limit + frame fidelity.
        port("recursion", "ext_soak/recursion.c").out("soak recursion total=108018000000\n"),

        // --- arithmetic-opcode endurance (long deterministic loops) ---
        // 30M-iter dependent FP chains (sqrt recurrence + mul/add/div), FP_CONTRACT OFF, bit-pattern output
        // -> exact IEEE determinism across x86-64 / aarch64 / darwin over a long run.
        port("fpaccum", "ext_soak/fpaccum.c").out("soak fpaccum xb=4600877379321698713 yb=4662744287908813514\n"),
        // 80M iters of popcount/ctz/clz/bswap/rotate/var-shift -> bit-manip opcode lowering held correct long-run.
        // xfail x86_64: the x86 JIT aborts with `UNIMPL 1B opcode 0xd3` on the REX.W variable rotate-by-CL
        // (`48 d3 c7` = ROL r/m64,CL) emitted by the rotate -> empty stdout. Repeatable. GAPS: jit86-rol-cl.
        port("bitchurn", "ext_soak/bitchurn.c").out("soak bitchurn acc=6137591891136901218\n"),
        // 60M signed+unsigned 64-bit div/mod with varying operands -> the one non-trivial integer op, long-run.
        port("divchurn", "ext_soak/divchurn.c").out("soak divchurn uacc=8510538310492573036 sacc=-1951097741945098340\n"),

        // --- memory / page-fault / TLB churn ---
        // 400k mmap/munmap of varied anon regions, both ends touched (~800k syscalls) -> map-bookkeeping leak.
        port("mmapchurn", "ext_soak/mmapchurn.c").out("soak mmapchurn sum=101993718\n"),
        // 2M realloc grow/shrink/relocate cycles on one live block, marker byte checked -> heap relocate path.
        port("reallocchurn", "ext_soak/reallocchurn.c").out("soak reallocchurn sum=131312824267\n"),
        // 3000x (mmap 2MB + first-touch every page + munmap) = ~1.5M demand faults -> fault handler + shadow map.
        port("pagefault", "ext_soak/pagefault.c").out("soak pagefault sum=192448512\n"),
        // 32MB buffer swept with a window that grows to full then shrinks -> oscillating working set, no syscalls.
        port("workingset", "ext_soak/workingset.c").out("soak workingset sum=459259904\n"),

        // --- concurrency endurance (futex / atomics) ---
        // 8 workers + producer, 1M tasks through a condvar+mutex queue -> millions of futex ops / lock hand-offs.
        port("threadpool", "ext_soak/threadpool.c").out("soak threadpool total=50999950\n"),
        // 8 threads x 2M contended fetch-add + CAS-retry (~16M RMW) -> atomic LL/SC / LOCK lowering + ordering.
        port("atomicchurn", "ext_soak/atomicchurn.c").out("soak atomicchurn counter=16000000 cas=16000000\n"),

        // --- process / IPC / signal / control-flow endurance ---
        // 2000x fork + pipe payload + read + reap (no exec -> dodges the fork+exec gap) -> fd/pid table churn.
        port("forkpipe", "ext_soak/forkpipe.c").out("soak forkpipe reaped=2000 sum=764168\n"),
        // 800k synchronous raise(SIGUSR1) under load -> deliver/save-context/run-handler/sigreturn, back-to-back.
        port("signalload", "ext_soak/signalload.c").out("soak signalload hits=800000 work=1001168286810961024\n"),
        // 4M setjmp+longjmp transfers, callee-saved value live across the jump -> non-local restore fidelity.
        port("longjmp", "ext_soak/longjmp.c").out("soak longjmp sum=506000000 work=1\n"),

        // --- self-modifying-code re-translation (hardest DBT endurance path) ---
        // 200k x patch TWO aarch64 instrs (movz+movk) at one RWX address, flush, call -> unbounded distinct
        // re-translations of the same block. aarch64 machine code only -> oracle (diff vs native).
        // xfail aarch64: documented SMC gap; mmap(RWX) is also EPERM on darwin W^X (no MAP_JIT) — see GAPS.md.
        src("smc2", "ext_soak/smc2.c").only(&[Engine::LinuxAarch64]).oracle(),
    ])
}

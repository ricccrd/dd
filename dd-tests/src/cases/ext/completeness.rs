//! completeness — systematic syscall-table + opcode-space coverage. Owner: completeness agent.
//! Goal: prove which syscalls and which x86-64/aarch64 instructions the engine handles vs leaves
//! UNIMPL/unhandled. Uses COMPILED GUESTS (no docker images → zero disk cost). Edit ONLY this file.
//!
//! Two systematic suites:
//!  1. SYSCALL COMPLETENESS — each guest drives one syscall (or a tight family) via direct
//!     `syscall(SYS_x, …)` with safe deterministic args and prints a stable verdict, then `.oracle()`:
//!     the JIT run's stdout+exit must equal the same guest run natively (aarch64 direct, x86_64 via
//!     qemu). If the engine returns -ENOSYS / a wrong value / diverges, the verdict differs and the
//!     oracle catches it → that's an unhandled/buggy syscall, marked `.xfail()` + a docs/GAPS.md row.
//!  2. OPCODE COMPLETENESS — x86-64 guests (compiled by the x86_64 cross-gcc, run on LinuxX86_64, oracle
//!     vs qemu) and aarch64 guests (native gcc, run on LinuxAarch64, oracle vs native) compute a
//!     deterministic value over fixed inputs across the SIMD / crypto / bitmanip / atomics instruction
//!     space via `__attribute__((target(...)))` intrinsics + inline asm. A mistranslation (wrong value)
//!     OR an UNIMPL (crash/diag) diverges from the oracle and is caught.
//!
//! All guests live under guests/completeness/<name>.c and share guests/completeness/compat.h.
#![allow(unused_imports)]
use crate::{group, src, port, fixture, in_rootfs, Case, Engine, Group};

/// syscall guest: one source, run on BOTH Linux engines, oracle-diffed vs native.
fn sy(name: &'static str, file: &'static str) -> Case { src(name, file).oracle() }
/// x86-64 opcode guest: x86_64 engine only (x86 intrinsics don't compile for aarch64), oracle vs qemu.
fn x(name: &'static str, file: &'static str) -> Case {
    src(name, file).only(&[Engine::LinuxX86_64]).oracle()
}
/// aarch64 opcode guest: aarch64 engine only, oracle vs native.
fn a(name: &'static str, file: &'static str) -> Case {
    src(name, file).only(&[Engine::LinuxAarch64]).oracle()
}
const X86: &[Engine] = &[Engine::LinuxX86_64];
const ARM: &[Engine] = &[Engine::LinuxAarch64];

pub fn groups() -> Vec<Group> {
    vec![
        // ---- SYSCALL COMPLETENESS ----
        sys_file(), sys_proc(), sys_mem(), sys_signal(), sys_time(), sys_cred(), sys_fs(), sys_misc(),
        // ---- OPCODE COMPLETENESS ----
        op_x86_sse(), op_x86_avx(), op_x86_bit(), op_x86_crypto(), op_x86_misc(),
        op_arm_neon(), op_arm_ext(),
    ]
}

// ===================== SYSCALL COMPLETENESS =====================

/// File-family syscalls: the at-suffixed / extended file ops a modern libc emits.
fn sys_file() -> Group {
    group("comp-sys-file", vec![
        // GAP openat2: engine opens a fd (ok=1) but the read returns \0 not the file's byte — openat2
        // doesn't honor the open_how flags / opens the wrong backing fd. (jit byte=\0 vs native byte=Z)
        sy("openat2",          "completeness/sys_openat2.c"),
        sy("statx",            "completeness/sys_statx.c"),
        sy("faccessat2",       "completeness/sys_faccessat2.c"),
        sy("readlinkat",       "completeness/sys_readlinkat.c"),
        sy("fchmodat",         "completeness/sys_fchmodat.c"),
        sy("utimensat",        "completeness/sys_utimensat.c"),
        sy("copy_file_range",  "completeness/sys_copy_file_range.c"),
        sy("fallocate",        "completeness/sys_fallocate.c"),
        // GAP name_to_handle_at: engine returns an error (ok=0) where real Linux succeeds (ok=1).
        sy("name_to_handle_at","completeness/sys_name_to_handle_at.c"),
        sy("truncate",         "completeness/sys_truncate.c"),
        sy("fsops",            "completeness/sys_fsops.c"),     // linkat/symlinkat/mkdirat/unlinkat
    ])
}

/// Process / scheduling / credentials-adjacent syscalls.
fn sys_proc() -> Group {
    group("comp-sys-proc", vec![
        // clone3 WORKS under both real engines; the x86_64 oracle (qemu-user) lacks clone3 so it
        // reports child=-1 → xfail X86 is an ORACLE artifact, not an engine gap (aarch64 passes).
        sy("clone3",          "completeness/sys_clone3.c").xfail(X86),
        sy("getrusage",       "completeness/sys_getrusage.c"),
        sy("prlimit64",       "completeness/sys_prlimit64.c"),
        sy("priority",        "completeness/sys_priority.c"),
        sy("getcpu",          "completeness/sys_getcpu.c"),
        sy("sched-affinity",  "completeness/sys_sched_affinity.c"),
        // GAP sched_get_priority_min(SCHED_FIFO): engine returns 0, real Linux returns 1.
        sy("sched-attr",      "completeness/sys_sched_attr.c"),
        sy("capget",          "completeness/sys_capget.c"),
        sy("set-tid-address", "completeness/sys_set_tid_address.c"),
    ])
}

/// Memory-management syscalls.
fn sys_mem() -> Group {
    group("comp-sys-mem", vec![
        sy("mremap",         "completeness/sys_mremap.c"),
        sy("mlock",          "completeness/sys_mlock.c"),
        sy("madvise",        "completeness/sys_madvise2.c"),       // WILLNEED/SEQUENTIAL/FREE
        // GAP mincore: engine returns an error (ok=0); real Linux fills the residency vector (ok=1).
        sy("mincore",        "completeness/sys_mincore.c"),
        // GAP membarrier: CMD_QUERY and CMD_GLOBAL both fail under the engine (query_ok=0 global=0).
        sy("membarrier",     "completeness/sys_membarrier.c"),
        // process_vm_readv now works on both engines; the x86_64 qemu-user oracle lacks it (n=-1) so the
        // JIT's correct result (n=16) reads as a mismatch -> oracle artifact, not an engine gap (cf. clone3).
        sy("process-vm",     "completeness/sys_process_vm.c").xfail(X86),
    ])
}

/// Signal-delivery / disposition syscalls.
fn sys_signal() -> Group {
    group("comp-sys-signal", vec![
        sy("rt-sigtimedwait", "completeness/sys_rt_sigtimedwait.c"),
        sy("sigaltstack",     "completeness/sys_sigaltstack.c"),
        sy("rt-sigpending",   "completeness/sys_rt_sigpending.c"),
        // GAP pidfd_open / pidfd_send_signal: engine returns an error from pidfd_open (open_ok=0);
        // real Linux opens a pidfd and signal-0 existence-check succeeds.
        sy("pidfd-signal",    "completeness/sys_pidfd_signal.c"),
    ])
}

/// Time / clock / timer syscalls.
fn sys_time() -> Group {
    group("comp-sys-time", vec![
        sy("clock-getres",   "completeness/sys_clock_getres.c"),
        sy("clock-variants", "completeness/sys_clock_variants.c"), // PROCESS/THREAD CPUTIME, BOOTTIME, RAW
        sy("timer-create",   "completeness/sys_timer_create.c"),
        sy("itimer",         "completeness/sys_itimer.c"),
        // GAP adjtimex/clock_adjtime: read-only (modes=0) query returns an error under the engine.
        sy("adjtimex",       "completeness/sys_adjtimex.c"),
    ])
}

/// Credential / identity syscalls (verdicts are structural so they're host-uid-invariant).
fn sys_cred() -> Group {
    group("comp-sys-cred", vec![
        sy("getresuid", "completeness/sys_getresuid.c"),
        sy("setfsuid",  "completeness/sys_setfsuid.c"),
        sy("getgroups", "completeness/sys_getgroups.c"),
    ])
}

/// Filesystem sync / stat / lock / ioctl syscalls.
fn sys_fs() -> Group {
    group("comp-sys-fs", vec![
        sy("fstatfs",      "completeness/sys_fstatfs.c"),
        // GAP syncfs: engine returns an error (syncfs=0) while fsync/fdatasync/sync_file_range all work.
        sy("sync-family",  "completeness/sys_sync_family.c"),
        sy("ioctl-fio",    "completeness/sys_ioctl_fio.c"),     // FIONREAD/FIONBIO
        sy("flock",        "completeness/sys_flock.c"),
        sy("fadvise",      "completeness/sys_fadvise.c"),
    ])
}

/// Random / system-info / misc syscalls.
fn sys_misc() -> Group {
    group("comp-sys-misc", vec![
        sy("getrandom-flags", "completeness/sys_getrandom_flags.c"),
        sy("getentropy",      "completeness/sys_getentropy.c"),
        // GAP auxv: AT_PAGESZ leaks the macOS host 16K page on aarch64 (jit 16384 vs native 4096);
        // the x86_64 engine now reports the correct auxv vector. Synthetic auxv vector is wrong on aarch64.
        sy("auxval",          "completeness/sys_auxval.c").xfail(ARM),
        sy("sysconf-nproc",   "completeness/sys_sysconf_nproc.c"),
        // GAP close_range: engine returns an error (ok=0); real Linux closes the fd range (ok=1).
        sy("close-range",     "completeness/sys_close_range.c"),
    ])
}

// ===================== OPCODE COMPLETENESS — x86-64 =====================

/// SSE/SSE2/SSE3/SSSE3/SSE4.1/SSE4.2 packed-int / packed-fp / shuffle / string ops.
fn op_x86_sse() -> Group {
    group("comp-x86-sse", vec![
        x("sse2",  "completeness/x86_sse2.c"),
        x("sse3",  "completeness/x86_sse3.c"),
        x("ssse3", "completeness/x86_ssse3.c"), // jit86 UNIMPL 0F 38 1C (PABSB) abort
        x("sse41", "completeness/x86_sse41.c"), // jit86 UNIMPL 0F 3A 40 (DPPS) abort
        x("sse42", "completeness/x86_sse42.c"), // jit86 UNIMPL 0F 38 F1 (CRC32 r/m) abort
    ])
}

/// AVX / AVX2 / FMA / F16C 256-bit vector lane.
fn op_x86_avx() -> Group {
    group("comp-x86-avx", vec![
        // The entire VEX-encoded (AVX) lane is unimplemented: jit86 UNIMPL 1B opcode 0xC5/0xC4
        // (2- and 3-byte VEX prefixes) → abort on the first 256-bit op.
        x("avx",  "completeness/x86_avx.c"),  // jit86 UNIMPL VEX 0xC5
        x("avx2", "completeness/x86_avx2.c"), // jit86 UNIMPL VEX 0xC5
        x("fma",  "completeness/x86_fma.c"),  // jit86 UNIMPL VEX 0xC4 (3-byte)
        x("f16c", "completeness/x86_f16c.c"), // jit86 UNIMPL VEX 0xC5
    ])
}

/// Bit-manipulation extensions: BMI1/BMI2, POPCNT, LZCNT, ADX, plus bit-test / double-shift inline asm.
fn op_x86_bit() -> Group {
    group("comp-x86-bit", vec![
        x("bmi1",    "completeness/x86_bmi1.c"),
        x("bmi2",    "completeness/x86_bmi2.c"),
        x("popcnt",  "completeness/x86_popcnt.c"),
        x("lzcnt",   "completeness/x86_lzcnt.c"),
        x("adx",     "completeness/x86_adx.c"),
        x("bittest", "completeness/x86_bittest.c"),   // bt/bts/btr/btc
        x("shld",    "completeness/x86_shld.c"),      // shld/shrd
    ])
}

/// Crypto / checksum: AES-NI, PCLMULQDQ, SHA-NI, CRC32 (SSE4.2).
fn op_x86_crypto() -> Group {
    group("comp-x86-crypto", vec![
        x("aesni",  "completeness/x86_aesni.c"),  // jit86 UNIMPL 0F 38 DC (AESENC) abort
        x("pclmul", "completeness/x86_pclmul.c"), // jit86 UNIMPL 0F 3A 44 (PCLMULQDQ) abort
        x("sha",    "completeness/x86_sha.c"),    // jit86 UNIMPL 0F 3A CC (SHA1RNDS4) abort
        x("crc32",  "completeness/x86_crc32.c"),  // jit86 UNIMPL 0F 38 F0 (CRC32 r/m8) abort
    ])
}

/// Misc instruction corners: MOVBE, CMPXCHG16B, RDTSC/RDTSCP, x87, REP string ops, non-temporal stores.
fn op_x86_misc() -> Group {
    group("comp-x86-misc", vec![
        x("movbe",       "completeness/x86_movbe.c"),      // jit86 UNIMPL 0F 38 F0 (MOVBE)
        x("cmpxchg16b",  "completeness/x86_cmpxchg16b.c"), // jit86 UNIMPL 0F C7 /1 (CMPXCHG16B)
        x("rdtsc",       "completeness/x86_rdtsc.c"),      // jit86 UNIMPL 0F 01 F9 (RDTSCP); rdtsc(0F31) ok
        x("x87",         "completeness/x86_x87.c"),
        x("repstring",   "completeness/x86_repstring.c"),   // rep movs/stos/cmps — handled
        x("movnt",       "completeness/x86_movnt.c"),      // jit86 UNIMPL 0F E7 (MOVNTDQ)
    ])
}

// ===================== OPCODE COMPLETENESS — aarch64 =====================

/// NEON/ASIMD base: int/fp arithmetic, reductions, table lookup, shifts, widen/narrow, permute,
/// compare/select, abs/neg, min/max, int<->fp convert.
fn op_arm_neon() -> Group {
    group("comp-arm-neon", vec![
        a("int",    "completeness/arm_neon_int.c"),
        a("fp",     "completeness/arm_neon_fp.c"),
        a("reduce", "completeness/arm_neon_reduce.c"),
        a("tbl",    "completeness/arm_neon_tbl.c"),
        a("shift",  "completeness/arm_neon_shift.c"),
        a("widen",  "completeness/arm_neon_widen.c"),
        a("perm",   "completeness/arm_neon_perm.c"),
        a("cmp",    "completeness/arm_neon_cmp.c"),
        a("abs",    "completeness/arm_neon_abs.c"),
        a("minmax", "completeness/arm_neon_minmax.c"),
        a("cvt",    "completeness/arm_neon_cvt.c"),
        a("bitfield","completeness/arm_bitfield.c"),   // rbit/rev/clz (ACLE)
        a("ldpstp", "completeness/arm_ldpstp.c"),       // load-pair/store-pair forms
    ])
}

/// aarch64 extensions: crypto (AES/SHA1/SHA256), CRC32, LSE atomics, FP16, dot-product, i8mm, bf16.
fn op_arm_ext() -> Group {
    group("comp-arm-ext", vec![
        a("crypto-aes",    "completeness/arm_crypto_aes.c"),
        a("crypto-sha1",   "completeness/arm_crypto_sha1.c"),
        a("crypto-sha256", "completeness/arm_crypto_sha256.c"),
        a("crc32",         "completeness/arm_crc32.c"),
        a("lse",           "completeness/arm_lse.c"),       // LDADD/SWP/CAS/LDSET/LDCLR
        a("fp16",          "completeness/arm_fp16.c"),
        a("dotprod",       "completeness/arm_dotprod.c"),
        a("i8mm",          "completeness/arm_i8mm.c"),      // SMMLA matrix-multiply
        a("bf16",          "completeness/arm_bf16.c"),      // BFDOT
    ])
}

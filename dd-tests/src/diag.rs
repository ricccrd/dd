//! Diagnostics — turn a failed run into an actionable JIT bug report.
//!
//! The whole point of this harness is to help JIT-builder agents finish the engine, so a failure must
//! explain ITSELF: what went wrong, why, and the crash details. Engine diagnostics (`unhandled syscall
//! N`, `[jit86] UNIMPL opcode 0xNN`, `CRASHDBG` faulting rip, signals) print to the **daemon log**, not
//! the container's stdout — so [`diagnose`] scrapes BOTH the captured output and the daemon-log tail and
//! classifies the failure into a known bucket (mapped to docs/GAPS.md) with a one-line, copy-pasteable
//! summary plus the supporting evidence.

/// A classified failure signal found in engine/container output.
#[derive(Debug, Clone, PartialEq)]
pub enum Signal {
    MissingSyscall(u64),       // "unhandled syscall N"
    UnimplOpcode(String),      // "[jit86] UNIMPL 1B opcode 0x1c" / aarch64 "UNIMPL opcode 0x.."
    Signal(i32, &'static str), // process died on a signal (139→SIGSEGV, …)
    ExecLoader(String),        // "open: No such file" / "failed to map segment" / "libc.so.6"
    CpuTopology,               // tcmalloc "NumPossibleCPUs ... cpus.has_value()(false)"
    ImageRegister,             // daemon "No such image" after a successful pull
    DaemonUnreachable,         // "Cannot connect to the Docker daemon"
    Fault(String),             // CRASHDBG rip / fatal-fault dump line
}

impl Signal {
    /// Which GAPS.md bucket this maps to (for the diagnostic agent + the gap inventory).
    pub fn bucket(&self) -> &'static str {
        match self {
            Signal::MissingSyscall(_) => "syscall-missing (os/linux/service.c switch(nr))",
            Signal::UnimplOpcode(_) => "B amd64/aarch64 opcode (jit translator)",
            Signal::Signal(_, _) | Signal::Fault(_) => "A loader/codegen fault (guest crash)",
            Signal::ExecLoader(_) => "A loader/exec (non-PIE/static entry, ld.so map)",
            Signal::CpuTopology => "D cpu-topology syscall (sched_getaffinity / /sys cpu)",
            Signal::ImageRegister => "C daemon image store (post-pull registration)",
            Signal::DaemonUnreachable => "harness/daemon (socket not reachable)",
        }
    }
    fn describe(&self) -> String {
        match self {
            Signal::MissingSyscall(n) => format!("guest hit syscall {n} which the engine does not implement"),
            Signal::UnimplOpcode(op) => format!("translator hit an unimplemented opcode: {op}"),
            Signal::Signal(c, name) => format!("process killed by {name} (exit {c})"),
            Signal::ExecLoader(l) => format!("ELF loader/exec failed: {l}"),
            Signal::CpuTopology => "CPU enumeration returned empty (possible-CPU set) — tcmalloc/jemalloc abort".into(),
            Signal::ImageRegister => "image pulled but daemon then reports 'No such image' (not registered)".into(),
            Signal::DaemonUnreachable => "could not connect to the daemon socket".into(),
            Signal::Fault(l) => format!("fatal fault: {l}"),
        }
    }
}

/// Map a process exit code to a signal name (128 + signo), for crash interpretation.
fn signal_for_code(code: i32) -> Option<(i32, &'static str)> {
    match code {
        132 => Some((code, "SIGILL")), 133 => Some((code, "SIGTRAP")), 134 => Some((code, "SIGABRT")),
        135 => Some((code, "SIGBUS")), 136 => Some((code, "SIGFPE")), 137 => Some((code, "SIGKILL/OOM")),
        139 => Some((code, "SIGSEGV")), 124 => None /* timeout, handled separately */, _ => None,
    }
}

/// A full diagnosis of one failed (scenario, target) run.
pub struct Diagnosis {
    pub what: String,            // the check that failed / timeout / spawn error
    pub signals: Vec<Signal>,    // engine signals scraped from output + daemon log
    pub out_clip: String,        // container output (clipped)
    pub log_clip: String,        // daemon-log tail (clipped) — dd backend only
    pub timed_out: bool,
}

impl Diagnosis {
    /// One-line summary appended to the failure line in the report.
    pub fn summary(&self) -> String {
        if self.timed_out { return format!("{} · TIMED OUT (likely hang/infinite-loop in translation)", self.what); }
        match self.signals.first() {
            Some(s) => format!("{} · {} [{}]", self.what, s.describe(), s.bucket()),
            None => self.what.clone(),
        }
    }
    /// Full multi-line report (verbose / --explain).
    pub fn report(&self) -> String {
        let mut r = String::new();
        r.push_str(&format!("  what : {}\n", self.what));
        if self.timed_out { r.push_str("  why  : timed out — the guest hung (commonly a mistranslated loop or a blocked syscall)\n"); }
        for s in &self.signals {
            r.push_str(&format!("  why  : {}\n         bucket → {}\n", s.describe(), s.bucket()));
        }
        if self.signals.is_empty() && !self.timed_out {
            r.push_str("  why  : no engine signal found — likely a wrong-output divergence (compare vs the Real oracle)\n");
        }
        if !self.out_clip.is_empty() { r.push_str(&format!("  out  : {}\n", self.out_clip)); }
        if !self.log_clip.is_empty() { r.push_str(&format!("  log  : {}\n", self.log_clip)); }
        r
    }
}

fn clip(s: &str, n: usize) -> String {
    let one = s.replace('\n', " ⏎ ");
    if one.chars().count() > n { one.chars().take(n).collect::<String>() + " …" } else { one }
}

/// Scrape engine/container output + daemon-log tail for known failure signals and build a diagnosis.
/// `what` is the check-level failure (e.g. `lacks [PONG]`); `code` the process exit; `out` the container
/// stdout+stderr; `log` the daemon-log tail (empty for the Real backend).
pub fn diagnose(what: String, code: i32, out: &str, log: &str) -> Diagnosis {
    let mut signals = Vec::new();
    let hay = format!("{out}\n{log}");
    let timed_out = code == 124;

    for line in hay.lines() {
        if let Some(i) = line.find("unhandled syscall ") {
            if let Some(n) = line[i + 18..].split_whitespace().next().and_then(|t| t.trim().parse::<u64>().ok()) {
                let s = Signal::MissingSyscall(n);
                if !signals.contains(&s) { signals.push(s); }
            }
        }
        if line.contains("UNIMPL") && (line.contains("opcode") || line.contains("0x")) {
            let op = line.trim().to_string();
            if !signals.iter().any(|s| matches!(s, Signal::UnimplOpcode(_))) { signals.push(Signal::UnimplOpcode(clip(&op, 80))); }
        }
        if line.contains("No such file or directory") && (line.contains("open") || line.contains("exec") || line.contains("map segment"))
            || line.contains("failed to map segment") || line.contains("libc.so.6") {
            if !signals.iter().any(|s| matches!(s, Signal::ExecLoader(_))) { signals.push(Signal::ExecLoader(clip(line.trim(), 100))); }
        }
        if line.contains("NumPossibleCPUs") || line.contains("cpus.has_value") {
            if !signals.contains(&Signal::CpuTopology) { signals.push(Signal::CpuTopology); }
        }
        if line.contains("No such image") {
            if !signals.contains(&Signal::ImageRegister) { signals.push(Signal::ImageRegister); }
        }
        if line.contains("Cannot connect to the Docker daemon") {
            if !signals.contains(&Signal::DaemonUnreachable) { signals.push(Signal::DaemonUnreachable); }
        }
        if (line.contains("rip=") || line.to_lowercase().contains("fatal")) && line.contains("rip=") {
            if !signals.iter().any(|s| matches!(s, Signal::Fault(_))) { signals.push(Signal::Fault(clip(line.trim(), 100))); }
        }
    }
    // a crash signal from the exit code, if not already explained by a richer signal
    if let Some((c, name)) = signal_for_code(code) {
        if !signals.iter().any(|s| matches!(s, Signal::Signal(_, _) | Signal::Fault(_))) { signals.push(Signal::Signal(c, name)); }
    }

    Diagnosis { what, signals, out_clip: clip(out, 220), log_clip: clip(log, 220), timed_out }
}

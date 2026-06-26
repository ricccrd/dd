//! # ddjit — the dd VM-less JIT container runtime + its bindings.
//!
//! The JIT runs a Linux container by translating its code and servicing its syscalls in userspace — no
//! VM. The C runtime (`src/runtime/`) is compiled by `build.rs` into one codesigned binary per guest
//! architecture (`aarch64`, `x86_64`); this crate exposes those binaries plus the typed
//! [`SpawnConfig`] launch contract. `dd-daemon` (and any other front-end) depends on this crate.
//!
//! ```no_run
//! use ddjit::{Guest, SpawnConfig};
//! let mut cfg = SpawnConfig::new("/work", "/images/alpine/rootfs");
//! cfg.hostname = Some("box".into());
//! cfg.argv = vec!["/bin/sh".into(), "-c".into(), "echo hi".into()];
//! if let Some((prog, args)) = cfg.command(Guest::Aarch64) {
//!     // std::process::Command::new(prog).args(args).spawn()...
//!     let _ = (prog, args);
//! }
//! ```

use std::path::Path;

/// A guest CPU architecture the JIT can run. Each maps to a binary built by `build.rs`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Guest { Aarch64, X86_64 }

impl Guest {
    /// Parse a guest arch from common spellings (e.g. an OCI image's `Architecture`).
    pub fn from_str(s: &str) -> Option<Guest> {
        match s.to_ascii_lowercase().as_str() {
            "aarch64" | "arm64" | "arm64/v8" => Some(Guest::Aarch64),
            "x86_64" | "amd64" | "x86-64" => Some(Guest::X86_64),
            _ => None,
        }
    }
    pub fn as_str(self) -> &'static str { match self { Guest::Aarch64 => "aarch64", Guest::X86_64 => "x86_64" } }

    /// Absolute path to the JIT binary for this guest, or `None` if `build.rs` couldn't build it
    /// (missing toolchain / `mac` bridge). The path is baked in at compile time.
    pub fn jit_path(self) -> Option<&'static str> {
        let p = match self { Guest::Aarch64 => env!("DDJIT_AARCH64"), Guest::X86_64 => env!("DDJIT_X86_64") };
        if p.is_empty() { None } else { Some(p) }
    }
}

/// A published port: the host port forwards to the container port (`docker -p HOST:CONTAINER`).
#[derive(Clone, Debug)]
pub struct PortMap { pub host: u16, pub container: u16 }

/// A bind mount: a host directory mounted at a path inside the container (`docker -v HOST:CONTAINER`).
#[derive(Clone, Debug)]
pub struct Volume { pub container: String, pub host: String }

/// Everything needed to launch one container in the JIT. Mirrors the JIT's flag/env contract:
/// `--rootfs/--lower/--hostname/--mem-max/--pids-max/--uid/--gid/--publish` + `DDVOL`/`DD_NETNS` env.
#[derive(Clone, Debug, Default)]
pub struct SpawnConfig {
    /// Working directory on the host (where relative image/rootfs paths resolve).
    pub work_dir: String,
    /// The writable rootfs (the overlay UPPER, or a plain single rootfs).
    pub rootfs: String,
    /// Read-only overlay lower layers, highest-priority first (the OCI image layers).
    pub lowers: Vec<String>,
    /// Bind-mounted volumes.
    pub volumes: Vec<Volume>,
    /// Published ports (`-p`).
    pub publish: Vec<PortMap>,
    /// Private-loopback network namespace id (isolates 127.0.0.0/8). `None` = shared.
    pub netns: Option<String>,
    /// UTS hostname.
    pub hostname: Option<String>,
    /// cgroup `memory.max` in bytes (0 = unlimited).
    pub mem_max: u64,
    /// cgroup `pids.max` (0 = unlimited).
    pub pids_max: u32,
    /// USER-ns uid / gid (default: root = 0).
    pub uid: Option<u32>,
    pub gid: Option<u32>,
    /// Extra environment for the guest process.
    pub env: Vec<(String, String)>,
    /// The guest argv (entrypoint + args).
    pub argv: Vec<String>,
}

fn shq(s: &str) -> String {
    let mut o = String::with_capacity(s.len() + 2);
    o.push('\'');
    for c in s.chars() { if c == '\'' { o.push_str("'\\''"); } else { o.push(c); } }
    o.push('\'');
    o
}

impl SpawnConfig {
    pub fn new(work_dir: impl Into<String>, rootfs: impl Into<String>) -> Self {
        SpawnConfig { work_dir: work_dir.into(), rootfs: rootfs.into(), ..Default::default() }
    }

    /// The `bash -lc` script that launches the container in the given guest's JIT (env prefix + JIT
    /// flags + `--rootfs` + argv). Returns `None` if that guest's binary wasn't built.
    pub fn script(&self, guest: Guest) -> Option<String> {
        let jit = guest.jit_path()?;
        let mut env = String::new();
        if !self.volumes.is_empty() {
            let v = self.volumes.iter().map(|v| format!("{}:{}", v.container, v.host)).collect::<Vec<_>>().join(",");
            env += &format!("DDVOL={} ", shq(&v));
        }
        if let Some(ns) = &self.netns { env += &format!("DD_NETNS={} ", shq(ns)); }
        for (k, val) in &self.env { env += &format!("{}={} ", k, shq(val)); }

        let mut f = String::new();
        if let Some(h) = &self.hostname { if !h.is_empty() { f += &format!("--hostname {} ", shq(h)); } }
        if self.mem_max > 0 { f += &format!("--mem-max {} ", self.mem_max); }
        if self.pids_max > 0 { f += &format!("--pids-max {} ", self.pids_max); }
        if let Some(u) = self.uid { f += &format!("--uid {} ", u); }
        if let Some(g) = self.gid { f += &format!("--gid {} ", g); }
        for l in &self.lowers { f += &format!("--lower {} ", shq(l)); }
        if !self.publish.is_empty() {
            let p = self.publish.iter().map(|p| format!("{}:{}", p.host, p.container)).collect::<Vec<_>>().join(",");
            f += &format!("--publish {} ", shq(&p));
        }
        let cd = if self.work_dir.is_empty() { String::new() } else { format!("cd {} && ", shq(&self.work_dir)) };
        let argv = self.argv.iter().map(|a| shq(a)).collect::<Vec<_>>().join(" ");
        Some(format!("{cd}{env}{jit} {f}--rootfs {} {argv}", shq(&self.rootfs)))
    }

    /// (program, args) to spawn the container. On macOS runs `bash -lc <script>`; on a non-macOS dev
    /// host it goes through the `mac` bridge. `None` if the guest's binary wasn't built.
    pub fn command(&self, guest: Guest) -> Option<(String, Vec<String>)> {
        let script = self.script(guest)?;
        Some(if cfg!(target_os = "macos") {
            ("bash".into(), vec!["-lc".into(), script])
        } else {
            ("mac".into(), vec!["bash".into(), "-lc".into(), script])
        })
    }
}

/// True if the JIT binary for `guest` was built and exists.
pub fn available(guest: Guest) -> bool {
    guest.jit_path().map(|p| Path::new(p).exists()).unwrap_or(false)
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn guest_parse() {
        assert_eq!(Guest::from_str("amd64"), Some(Guest::X86_64));
        assert_eq!(Guest::from_str("arm64"), Some(Guest::Aarch64));
        assert_eq!(Guest::from_str("riscv"), None);
    }
    #[test]
    fn script_has_flags() {
        // jit_path may be empty in a host without the toolchain; script() returns None then. Guard.
        let mut c = SpawnConfig::new("/work", "img/upper");
        c.lowers = vec!["img/l0".into()];
        c.hostname = Some("box".into());
        c.mem_max = 256 << 20;
        c.publish = vec![PortMap { host: 18080, container: 80 }];
        c.volumes = vec![Volume { container: "/data".into(), host: "/h".into() }];
        c.argv = vec!["/bin/sh".into()];
        if let Some(s) = c.script(Guest::Aarch64) {
            assert!(s.contains("--rootfs 'img/upper'") && s.contains("--lower 'img/l0'"));
            assert!(s.contains("--hostname 'box'") && s.contains("--mem-max 268435456"));
            assert!(s.contains("--publish '18080:80'") && s.contains("DDVOL='/data:/h'"));
        }
    }
}

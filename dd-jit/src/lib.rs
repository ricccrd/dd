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
//! if let Some((prog, args)) = cfg.command(Guest::LinuxAarch64) {
//!     // std::process::Command::new(prog).args(args).spawn()...
//!     let _ = (prog, args);
//! }
//! ```

use std::path::Path;

/// A guest target = (OS personality, ISA) the JIT can run. Each maps to one binary built by `build.rs`
/// from `targets/<target>.c`. The OS axis is `linux` (jit / jit86) or `darwin` (jitdarwin — native
/// macOS Mach-O containers); the ISA axis is `aarch64` or `x86_64`.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Default)]
pub enum Guest { #[default] LinuxAarch64, LinuxX86_64, DarwinAarch64 }

impl Guest {
    pub const ALL: [Guest; 3] = [Guest::LinuxAarch64, Guest::LinuxX86_64, Guest::DarwinAarch64];

    /// Guest OS personality: `"linux"` or `"darwin"`.
    pub fn os(self) -> &'static str { match self { Guest::DarwinAarch64 => "darwin", _ => "linux" } }
    /// Guest instruction set: `"aarch64"` or `"x86_64"`.
    pub fn arch(self) -> &'static str { match self { Guest::LinuxX86_64 => "x86_64", _ => "aarch64" } }
    /// The build-target name, matching `build.rs` and `targets/<target>.c`.
    pub fn target(self) -> &'static str {
        match self { Guest::LinuxAarch64 => "linux_aarch64", Guest::LinuxX86_64 => "linux_x86_64", Guest::DarwinAarch64 => "darwin_aarch64" }
    }

    /// Pick a target from an OS + arch (e.g. detected from a binary's magic / an image's metadata).
    pub fn detect(os: &str, arch: &str) -> Option<Guest> {
        match (os, arch.to_ascii_lowercase().as_str()) {
            ("linux", "aarch64" | "arm64" | "arm64/v8") => Some(Guest::LinuxAarch64),
            ("linux", "x86_64" | "amd64" | "x86-64") => Some(Guest::LinuxX86_64),
            ("darwin", "aarch64" | "arm64") => Some(Guest::DarwinAarch64),
            _ => None,
        }
    }

    /// Absolute path to the JIT binary, or `None` if it can't be located.
    ///
    /// Resolution order: a runtime `$DDJIT_DIR/ddjit-<target>` (used by the packaged `.app`, whose
    /// LaunchAgent points `DDJIT_DIR` at `Contents/Resources`), then the path `build.rs` baked in at
    /// compile time (the dev/`cargo` layout). Returns `None` if neither exists.
    pub fn jit_path(self) -> Option<String> {
        if let Ok(dir) = std::env::var("DDJIT_DIR") {
            let cand = format!("{dir}/ddjit-{}", self.target());
            if Path::new(&cand).exists() {
                return Some(cand);
            }
        }
        let p = match self {
            Guest::LinuxAarch64 => env!("DDJIT_LINUX_AARCH64"),
            Guest::LinuxX86_64 => env!("DDJIT_LINUX_X86_64"),
            Guest::DarwinAarch64 => env!("DDJIT_DARWIN_AARCH64"),
        };
        if p.is_empty() { None } else { Some(p.to_string()) }
    }

    /// Path to the darwinjail interposing dylib (DYLD_INSERT) that runs native macOS arm64 binaries in a
    /// container. `DDJIT_DIR/darwinjail.dylib` (bundle) wins, else the build-time path. `None` if absent.
    pub fn jail_dylib(&self) -> Option<String> {
        if let Ok(dir) = std::env::var("DDJIT_DIR") {
            let cand = format!("{dir}/darwinjail.dylib");
            if Path::new(&cand).exists() {
                return Some(cand);
            }
        }
        let p = env!("DDJAIL_DARWIN_AARCH64");
        if p.is_empty() { None } else { Some(p.to_string()) }
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

    /// The `bash -lc` script that launches the container in the given guest's JIT. The flag/env contract
    /// differs per guest OS — linux (jit/jit86) takes the full container flag set + `DDVOL`/`DD_NETNS`
    /// env; darwin (jitdarwin) takes `--rootfs` + `--volume HOST:CONT`. Returns `None` if not built.
    pub fn script(&self, guest: Guest) -> Option<String> {
        let cd = if self.work_dir.is_empty() { String::new() } else { format!("cd {} && ", shq(&self.work_dir)) };
        let argv = self.argv.iter().map(|a| shq(a)).collect::<Vec<_>>().join(" ");
        let body = if guest.os() == "darwin" {
            // darwinjail: run the native arm64 binary jailed via an interposing dylib (DYLD_INSERT) -- no
            // DBT. The container model (rootfs/lowers/volumes/hostname/limits/publish) is passed as env.
            let jail = guest.jail_dylib()?;
            let mut env = format!("DYLD_INSERT_LIBRARIES={} DD_SANDBOX=1 ", shq(&jail));
            if !self.rootfs.is_empty() { env += &format!("DD_ROOTFS={} ", shq(&self.rootfs)); }
            if !self.lowers.is_empty() { env += &format!("DD_LOWERS={} ", shq(&self.lowers.join(","))); }
            if !self.volumes.is_empty() {
                let v = self.volumes.iter().map(|v| format!("{}:{}", v.host, v.container)).collect::<Vec<_>>().join(",");
                env += &format!("DD_VOLUMES={} ", shq(&v));
            }
            if let Some(h) = &self.hostname { if !h.is_empty() { env += &format!("DD_HOSTNAME={} ", shq(h)); } }
            if self.mem_max > 0 { env += &format!("DD_MEM_MAX={} ", self.mem_max); }
            if self.pids_max > 0 { env += &format!("DD_PIDS_MAX={} ", self.pids_max); }
            if !self.publish.is_empty() {
                let p = self.publish.iter().map(|p| format!("{}:{}", p.host, p.container)).collect::<Vec<_>>().join(",");
                env += &format!("DD_PUBLISH={} ", shq(&p));
            }
            for (k, val) in &self.env { env += &format!("{}={} ", k, shq(val)); }
            // `exec env …` so the container process REPLACES this shell -- it becomes the session leader /
            // foreground of the PTY, so an interactive shell can read the terminal (no job-control stall).
            format!("exec env {env}{argv}")
        } else {
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
            // A bare static-PIE guest needs no rootfs; omit the flag so it runs un-jailed.
            if !self.rootfs.is_empty() { f += &format!("--rootfs {} ", shq(&self.rootfs)); }
            // `exec env …` so the JIT REPLACES the wrapper shell -- it becomes the process the daemon
            // tracks (live.pid), so `docker pause` (SIGSTOP/SIGCONT) hits the JIT, not a dead bash parent.
            format!("exec env {env}{jit} {f}{argv}")
        };
        Some(format!("{cd}{body}"))
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
    guest.jit_path().map(|p| Path::new(&p).exists()).unwrap_or(false)
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn guest_detect() {
        assert_eq!(Guest::detect("linux", "amd64"), Some(Guest::LinuxX86_64));
        assert_eq!(Guest::detect("linux", "arm64"), Some(Guest::LinuxAarch64));
        assert_eq!(Guest::detect("darwin", "aarch64"), Some(Guest::DarwinAarch64));
        assert_eq!(Guest::detect("plan9", "aarch64"), None);
        assert_eq!(Guest::DarwinAarch64.os(), "darwin");
        assert_eq!(Guest::LinuxX86_64.arch(), "x86_64");
    }
    #[test]
    fn linux_script_has_full_flags() {
        // jit_path may be empty in a host without the toolchain; script() returns None then. Guard.
        let mut c = SpawnConfig::new("/work", "img/upper");
        c.lowers = vec!["img/l0".into()];
        c.hostname = Some("box".into());
        c.mem_max = 256 << 20;
        c.publish = vec![PortMap { host: 18080, container: 80 }];
        c.volumes = vec![Volume { container: "/data".into(), host: "/h".into() }];
        c.argv = vec!["/bin/sh".into()];
        if let Some(s) = c.script(Guest::LinuxAarch64) {
            assert!(s.contains("--rootfs 'img/upper'") && s.contains("--lower 'img/l0'"));
            assert!(s.contains("--hostname 'box'") && s.contains("--mem-max 268435456"));
            assert!(s.contains("--publish '18080:80'") && s.contains("DDVOL='/data:/h'"));
        }
    }
    #[test]
    fn darwin_script_uses_rootfs_volume() {
        let mut c = SpawnConfig::new("", "/jail");
        c.volumes = vec![Volume { container: "/data".into(), host: "/h".into() }];
        c.argv = vec!["/bin/app".into()];
        if let Some(s) = c.script(Guest::DarwinAarch64) {
            assert!(s.contains("--rootfs '/jail'") && s.contains("--volume '/h':'/data'"));
            assert!(!s.contains("--mem-max") && !s.contains("DDVOL")); // darwin contract is leaner
        }
    }
}

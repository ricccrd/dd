//! dd-tests — a declarative test harness that runs guest programs across every JIT engine.
//!
//! A [`Case`] is one guest program + its expected behaviour. Cases are organised into named
//! [`Group`]s. The runner executes the **engine × case** matrix: each case runs on every engine whose
//! guest architecture it can be provisioned for (aarch64 guests are compiled on the fly; x86-64 guests
//! come from prebuilt fixtures, since there's no local cross-compiler). Checks are golden
//! (exit/stdout) or differential against a native oracle.
//!
//! ```ignore
//! group("compat", [
//!     src("hello", "hello.c").exit(42).out("hi\n"),
//!     src("sort",  "sort.c").oracle(),                 // diff vs native run
//! ])
//! ```

use std::path::PathBuf;
use std::process::Command;

pub mod cases;

/// A JIT engine = one guest target (OS × ISA) the runtime can execute.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Engine { LinuxAarch64, LinuxX86_64, DarwinAarch64 }

impl Engine {
    pub const ALL: [Engine; 3] = [Engine::LinuxAarch64, Engine::LinuxX86_64, Engine::DarwinAarch64];
    pub fn jit(self) -> ddjit::Guest {
        match self {
            Engine::LinuxAarch64 => ddjit::Guest::LinuxAarch64,
            Engine::LinuxX86_64 => ddjit::Guest::LinuxX86_64,
            Engine::DarwinAarch64 => ddjit::Guest::DarwinAarch64,
        }
    }
    pub fn os(self) -> &'static str { self.jit().os() }
    pub fn arch(self) -> &'static str { self.jit().arch() }
    /// Display label that disambiguates same-ISA targets (e.g. linux/aarch64 vs darwin/aarch64).
    pub fn label(self) -> String { format!("{}/{}", self.os(), self.arch()) }
    /// Whether this engine's JIT binary was built (by dd-jit's build.rs).
    pub fn available(self) -> bool { ddjit::available(self.jit()) }
    /// Whether guest binaries for this target can be compiled locally (only linux/aarch64 via native gcc).
    pub fn can_compile(self) -> bool { matches!(self, Engine::LinuxAarch64 | Engine::LinuxX86_64) }
}

/// How a case's guest binary is obtained for a given engine.
pub enum Bin {
    /// Compile a Linux C source under `guests/` (gcc -static-pie, per Linux arch). Linux engines only.
    Source(&'static str),
    /// A portable POSIX C source under `guests/` built for *every* engine: gcc -static-pie for the two
    /// Linux engines, clang (full libSystem) Mach-O for darwin. The one source proves the behaviour is
    /// identical on Linux (JIT-emulated) and macOS (native under darwinjail) — so coverage isn't
    /// Linux-only. Checks must be golden (deterministic stdout/exit), since darwin has no native oracle.
    Portable(&'static str),
    /// Compile a macOS/aarch64 Mach-O C source under `guests/darwin/` (mac clang).
    DarwinSource(&'static str),
    /// A macOS-only C source (path relative to `guests/`, e.g. `darwin/kqueue.c`) built with the full
    /// libSystem (normal C runtime + main) and run on the darwin engine. For BSD/Mach-only APIs
    /// (kqueue, sysctl, mach_*) that have no Linux form — the darwin counterpart to a Linux-only `src`.
    DarwinLibc(&'static str),
    /// Prebuilt fixture binaries, one per engine that has one.
    Fixture(&'static [(Engine, &'static str)]),
    /// The guest program is already inside the rootfs; `args[0]` names it (e.g. `/bin/sh`).
    InRootfs,
}

/// A single expectation, evaluated against the JIT run.
pub enum Check {
    Exit(i32),
    Out(&'static str),
    OutHas(&'static str),
    /// Run the same guest natively and require identical stdout + exit (aarch64 source guests only).
    Oracle,
}

/// One test: a guest program + how to launch it + what to expect.
pub struct Case {
    pub name: &'static str,
    pub bin: Bin,
    pub args: Vec<String>,
    pub rootfs: Option<&'static str>, // image name (resolved per-arch) or None = bare
    pub lowers: Vec<String>,
    pub mem_max: u64,
    pub engines: Vec<Engine>,
    /// Engines where this case is a KNOWN failure (jit86 translator/service bugs under debugging) — a
    /// fail there is reported `xfail`, not a regression.
    pub xfail: Vec<Engine>,
    pub checks: Vec<Check>,
}

/// A named collection of cases.
pub struct Group { pub name: &'static str, pub cases: Vec<Case> }

pub fn group(name: &'static str, cases: Vec<Case>) -> Group { Group { name, cases } }

// ---- ergonomic builders ----
fn base(name: &'static str, bin: Bin) -> Case {
    let engines = match &bin {
        Bin::Source(_) => vec![Engine::LinuxAarch64, Engine::LinuxX86_64], // same source, both Linux engines
        Bin::Portable(_) => Engine::ALL.to_vec(),                          // every engine: Linux x2 + darwin
        Bin::DarwinSource(_) => vec![Engine::DarwinAarch64],
        Bin::DarwinLibc(_) => vec![Engine::DarwinAarch64],
        Bin::Fixture(fx) => fx.iter().map(|(e, _)| *e).collect(),
        Bin::InRootfs => vec![Engine::LinuxAarch64], // container rootfs fixtures are aarch64 today
    };
    Case { name, bin, args: vec![], rootfs: None, lowers: vec![], mem_max: 0, engines, xfail: vec![], checks: vec![] }
}
/// A case whose guest is compiled from a Linux/aarch64 C source under `guests/`.
pub fn src(name: &'static str, source: &'static str) -> Case { base(name, Bin::Source(source)) }
/// A case whose guest is a portable POSIX source under `guests/`, run on EVERY engine (Linux x2 +
/// darwin). Use golden checks — the same deterministic output must appear on Linux and macOS.
pub fn port(name: &'static str, source: &'static str) -> Case { base(name, Bin::Portable(source)) }
/// A case whose guest is compiled from a macOS/aarch64 Mach-O C source under `guests/darwin/`.
pub fn darwin_src(name: &'static str, source: &'static str) -> Case { base(name, Bin::DarwinSource(source)) }
/// A macOS-only case (source path relative to `guests/`, e.g. `darwin/kqueue.c`), full-libSystem, run
/// on the darwin engine only. For BSD/Mach APIs with no Linux equivalent. Golden-checked.
pub fn darwin_libc(name: &'static str, source: &'static str) -> Case { base(name, Bin::DarwinLibc(source)) }
/// A case whose guest is a prebuilt fixture, per engine.
pub fn fixture(name: &'static str, fx: &'static [(Engine, &'static str)]) -> Case { base(name, Bin::Fixture(fx)) }
/// A case that runs a program already inside the rootfs (e.g. busybox); `a` is the full argv.
pub fn in_rootfs(name: &'static str, rootfs: &'static str, a: &[&str]) -> Case {
    let mut c = base(name, Bin::InRootfs);
    c.rootfs = Some(rootfs);
    c.args = a.iter().map(|s| s.to_string()).collect();
    c
}

impl Case {
    pub fn arg(mut self, a: &str) -> Self { self.args.push(a.into()); self }
    pub fn args(mut self, a: &[&str]) -> Self { self.args.extend(a.iter().map(|s| s.to_string())); self }
    pub fn rootfs(mut self, r: &'static str) -> Self { self.rootfs = Some(r); self }
    pub fn lower(mut self, l: &str) -> Self { self.lowers.push(l.into()); self }
    pub fn mem(mut self, m: u64) -> Self { self.mem_max = m; self }
    pub fn only(mut self, e: &[Engine]) -> Self { self.engines = e.to_vec(); self }
    pub fn exit(mut self, c: i32) -> Self { self.checks.push(Check::Exit(c)); self }
    pub fn out(mut self, s: &'static str) -> Self { self.checks.push(Check::Out(s)); self }
    pub fn has(mut self, s: &'static str) -> Self { self.checks.push(Check::OutHas(s)); self }
    pub fn oracle(mut self) -> Self { self.checks.push(Check::Oracle); self }
    /// Mark this case a KNOWN failure on the given engines (jit86 bugs under debugging): a fail there
    /// is reported `xfail` (not a regression); an unexpected pass is reported `XPASS`.
    pub fn xfail(mut self, e: &[Engine]) -> Self { self.xfail = e.to_vec(); self }
}

/// Result of running one case on one engine.
pub enum Status {
    Pass,
    Fail(String),
    Skip(String),
    /// A KNOWN failure (the case is `.xfail()`-marked here) — tracked, not a regression.
    Xfail(String),
    /// An xfail-marked case that unexpectedly PASSED — the bug may be fixed; un-mark it.
    Xpass,
}

/// Shared paths/config for a run.
pub struct Ctx {
    pub repo: PathBuf,     // dd repo root (shared mount, visible to the mac-side JIT)
    pub guests: PathBuf,   // dd-tests/guests
    pub cache: PathBuf,    // compiled-guest cache (under target/, shared)
    pub images: PathBuf,   // image rootfs dir (default the poc images)
}

impl Ctx {
    pub fn discover() -> Ctx {
        let repo = PathBuf::from(env!("CARGO_MANIFEST_DIR")).parent().unwrap().to_path_buf();
        let cache = repo.join("target/dd-tests");
        std::fs::create_dir_all(cache.join("aarch64")).ok();
        Ctx {
            guests: PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("guests"),
            images: std::env::var("DD_IMAGES").map(PathBuf::from).unwrap_or_else(|_| PathBuf::from("/Users/x/dd/poc/images")),
            cache, repo,
        }
    }
    /// Resolve an image name to its rootfs path for the given arch (aarch64 today).
    fn rootfs_path(&self, name: &str, _e: Engine) -> Option<String> {
        let rd = std::fs::read_dir(&self.images).ok()?;
        for ent in rd.flatten() {
            let n = ent.file_name().to_string_lossy().to_string();
            if n.contains(name) && ent.path().join("rootfs").is_dir() {
                return Some(ent.path().join("rootfs").to_string_lossy().into_owned());
            }
        }
        None
    }
}

/// Compile a guest C source for a Linux engine. aarch64 = native gcc, x86_64 = the cross compiler; both
/// static-PIE, cached by mtime under cache/<arch>/. The same source runs on both engines (the point —
/// it makes the engine matrix dense). Returns the binary path.
fn compile(ctx: &Ctx, source: &str, e: Engine) -> Result<String, String> {
    let src = ctx.guests.join(source);
    let out = ctx.cache.join(e.arch()).join(source.trim_end_matches(".c"));
    std::fs::create_dir_all(out.parent().unwrap()).ok();
    let needs = !out.exists()
        || std::fs::metadata(&src).and_then(|m| m.modified()).ok()
            >= std::fs::metadata(&out).and_then(|m| m.modified()).ok();
    if needs {
        // aarch64: native gcc + libsqlite3/libdl (real-software guests). x86_64: the cross compiler,
        // libm only (no x86 libsqlite3 on the dev host). Static, unused libs aren't pulled.
        let (cc, libs): (&str, &[&str]) = match e {
            Engine::LinuxAarch64 => ("gcc", &["-lsqlite3", "-lm", "-ldl"]),
            Engine::LinuxX86_64 => ("x86_64-linux-gnu-gcc", &["-lm"]),
            _ => return Err(format!("{} is not a compilable Linux target", e.label())),
        };
        let o = Command::new(cc).args(["-O2", "-static-pie", "-pthread"])
            .arg("-o").arg(&out).arg(&src).args(libs).output()
            .map_err(|err| format!("{cc} spawn: {err}"))?;
        if !o.status.success() { return Err(format!("compile {source} [{}]: {}", e.arch(), String::from_utf8_lossy(&o.stderr).trim())); }
    }
    Ok(out.to_string_lossy().into_owned())
}

/// Provision the guest binary path for a case on an engine. `Ok(None)` = skip (no guest for this arch).
fn provision(ctx: &Ctx, c: &Case, e: Engine) -> Result<Option<String>, String> {
    match &c.bin {
        Bin::Source(s) if e.can_compile() => compile(ctx, s, e).map(Some),
        Bin::Source(_) => Ok(None),
        // portable POSIX: Linux engines via gcc (same as Source), darwin via clang+libSystem.
        Bin::Portable(s) if e.can_compile() => compile(ctx, s, e).map(Some),
        Bin::Portable(s) if e == Engine::DarwinAarch64 => compile_darwin_libc(ctx, s).map(Some),
        Bin::Portable(_) => Ok(None),
        Bin::DarwinSource(s) if e == Engine::DarwinAarch64 => compile_darwin(ctx, s).map(Some),
        Bin::DarwinSource(_) => Ok(None),
        Bin::DarwinLibc(s) if e == Engine::DarwinAarch64 => compile_darwin_libc(ctx, s).map(Some),
        Bin::DarwinLibc(_) => Ok(None),
        Bin::Fixture(fx) => Ok(fx.iter().find(|(fe, _)| *fe == e).map(|(_, p)| resolve(ctx, p))),
        Bin::InRootfs => Ok(Some(String::new())), // nothing to build; argv[0] is in-rootfs
    }
}

/// Compile a static macOS/arm64 Mach-O guest from `guests/darwin/<source>` via the mac toolchain.
/// (Darwin guests use a different syscall ABI than linux, so they're their own sources; checked golden
/// since they can't run natively on a linux dev host for an oracle.)
fn compile_darwin(ctx: &Ctx, source: &str) -> Result<String, String> {
    let src = ctx.guests.join("darwin").join(source);
    let out = ctx.cache.join("darwin").join(source.trim_end_matches(".c"));
    std::fs::create_dir_all(out.parent().unwrap()).ok();
    let fresh = std::fs::metadata(&out).and_then(|m| m.modified()).ok()
        >= std::fs::metadata(&src).and_then(|m| m.modified()).ok();
    if !out.exists() || !fresh {
        let script = format!("clang -arch arm64 -nostartfiles -e _start -o '{}' '{}' -lSystem",
            out.display(), src.display());
        let o = if cfg!(target_os = "macos") { Command::new("bash").arg("-lc").arg(&script).output() }
                else { Command::new("mac").arg("bash").arg("-lc").arg(&script).output() }
            .map_err(|e| format!("mac clang spawn: {e}"))?;
        if !o.status.success() { return Err(format!("compile darwin/{source}: {}", String::from_utf8_lossy(&o.stderr).trim())); }
    }
    Ok(out.to_string_lossy().into_owned())
}
/// Compile a *portable* guest from `guests/<source>` as a normal macOS/arm64 Mach-O linked against the
/// full libSystem (real C runtime + main), cached under cache/darwin/. Runs natively under darwinjail —
/// so the same POSIX source that runs on the Linux engines also runs (un-emulated) on macOS.
fn compile_darwin_libc(ctx: &Ctx, source: &str) -> Result<String, String> {
    let src = ctx.guests.join(source);
    let out = ctx.cache.join("darwin").join(source.trim_end_matches(".c"));
    std::fs::create_dir_all(out.parent().unwrap()).ok();
    let fresh = std::fs::metadata(&out).and_then(|m| m.modified()).ok()
        >= std::fs::metadata(&src).and_then(|m| m.modified()).ok();
    if !out.exists() || !fresh {
        let script = format!("clang -arch arm64 -O2 -o '{}' '{}'", out.display(), src.display());
        let o = if cfg!(target_os = "macos") { Command::new("bash").arg("-lc").arg(&script).output() }
                else { Command::new("mac").arg("bash").arg("-lc").arg(&script).output() }
            .map_err(|e| format!("mac clang spawn: {e}"))?;
        if !o.status.success() { return Err(format!("compile darwin(libc) {source}: {}", String::from_utf8_lossy(&o.stderr).trim())); }
    }
    Ok(out.to_string_lossy().into_owned())
}
fn resolve(ctx: &Ctx, p: &str) -> String {
    if p.starts_with('/') { p.into() } else { ctx.repo.parent().unwrap().join("poc").join(p).to_string_lossy().into_owned() }
}

/// Run one case on one engine and evaluate its checks.
pub fn run(ctx: &Ctx, c: &Case, e: Engine) -> Status {
    if !c.engines.contains(&e) { return Status::Skip("n/a for engine".into()); }
    if !e.available() { return Status::Skip(format!("{} JIT not built", e.label())); }
    let guest = match provision(ctx, c, e) {
        Ok(Some(g)) => g,
        Ok(None) => return Status::Skip(format!("no {} guest", e.label())),
        Err(err) => return Status::Fail(err),
    };
    let rootfs = c.rootfs.and_then(|r| ctx.rootfs_path(r, e));
    if c.rootfs.is_some() && rootfs.is_none() { return Status::Skip(format!("no {} rootfs", e.label())); }

    let mut cfg = ddjit::SpawnConfig::new(String::new(), rootfs.unwrap_or_default());
    cfg.lowers = c.lowers.clone();
    cfg.mem_max = c.mem_max;
    cfg.argv = match &c.bin {
        Bin::InRootfs => c.args.clone(),
        _ => std::iter::once(guest.clone()).chain(c.args.iter().cloned()).collect(),
    };
    let (prog, args) = match cfg.command(e.jit()) { Some(x) => x, None => return Status::Skip("no command".into()) };
    // Wrap in `timeout` so a hung/looping guest can't block the matrix (the x86 JIT can mistranslate
    // into an infinite loop). 124 = timed out.
    let out = match Command::new("timeout").arg("25").arg(&prog).args(&args).output() {
        Ok(o) => o,
        Err(err) => return Status::Fail(format!("spawn: {err}")),
    };
    // a known failure on this engine is reported xfail, not a regression
    let fail = |msg: String| if c.xfail.contains(&e) { Status::Xfail(msg) } else { Status::Fail(msg) };
    if out.status.code() == Some(124) { return fail(format!("timeout (>25s) [{}]", e.label())); }
    if std::env::var("DD_DEBUG").is_ok() {
        eprintln!("\n[dbg] {} {:?}\n[dbg] out={:?}\n[dbg] err={:?}\n[dbg] code={:?}", prog, args,
            String::from_utf8_lossy(&out.stdout), String::from_utf8_lossy(&out.stderr), out.status.code());
    }

    let stdout = strip_noise(&out.stdout);
    let code = out.status.code().unwrap_or(-1);
    for chk in &c.checks {
        if let Err(msg) = eval(chk, &stdout, code, &guest, &c.args, e) {
            if std::env::var("CRASHDBG").is_ok() {
                eprintln!("[crashdbg {}] code={code} stderr={}", e.label(),
                    String::from_utf8_lossy(&out.stderr).trim());
            }
            return fail(msg);
        }
    }
    if c.xfail.contains(&e) { Status::Xpass } else { Status::Pass }
}

fn eval(chk: &Check, stdout: &str, code: i32, guest: &str, args: &[String], e: Engine) -> Result<(), String> {
    match chk {
        Check::Exit(want) => (code == *want).then_some(()).ok_or_else(|| format!("exit {code} != {want}")),
        Check::Out(want) => (stdout == *want).then_some(()).ok_or_else(|| format!("stdout {:?} != {:?}", stdout, want)),
        Check::OutHas(sub) => stdout.contains(sub).then_some(()).ok_or_else(|| format!("stdout {:?} lacks {:?}", stdout, sub)),
        Check::Oracle => {
            // native ground truth: aarch64 runs directly; x86_64 runs under qemu-user.
            let o = match e {
                Engine::LinuxX86_64 => Command::new("timeout").arg("25").arg("qemu-x86_64").arg(guest).args(args).output(),
                _ => Command::new("timeout").arg("25").arg(guest).args(args).output(),
            }.map_err(|err| format!("oracle spawn: {err}"))?;
            let (eo, ec) = (strip_noise(&o.stdout), o.status.code().unwrap_or(-1));
            if eo != stdout || ec != code { Err(format!("oracle mismatch (jit {code}/{stdout:?} vs native {ec}/{eo:?})")) } else { Ok(()) }
        }
    }
}

/// Drop the JIT's diagnostic "unhandled syscall ..." lines so they don't pollute stdout checks.
fn strip_noise(b: &[u8]) -> String {
    String::from_utf8_lossy(b).lines().filter(|l| !l.contains("unhandled syscall")).collect::<Vec<_>>().join("\n")
        + if b.ends_with(b"\n") && !b.is_empty() { "\n" } else { "" }
}

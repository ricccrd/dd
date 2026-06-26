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

/// A JIT engine = a guest architecture the runtime can execute.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Engine { Aarch64, X86_64 }

impl Engine {
    pub const ALL: [Engine; 2] = [Engine::Aarch64, Engine::X86_64];
    pub fn arch(self) -> &'static str { match self { Engine::Aarch64 => "aarch64", Engine::X86_64 => "x86_64" } }
    pub fn jit(self) -> ddjit::Guest { match self { Engine::Aarch64 => ddjit::Guest::Aarch64, Engine::X86_64 => ddjit::Guest::X86_64 } }
    /// Whether this engine's JIT binary was built (by dd-jit's build.rs).
    pub fn available(self) -> bool { ddjit::available(self.jit()) }
    /// Whether guest binaries for this arch can be produced locally (compiled or by prebuilt fixture).
    pub fn can_compile(self) -> bool { self == Engine::Aarch64 } // native gcc on the aarch64 dev host
}

/// How a case's guest binary is obtained for a given engine.
pub enum Bin {
    /// Compile this C source (under `guests/`). Only arches with a local toolchain (aarch64).
    Source(&'static str),
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
    pub checks: Vec<Check>,
}

/// A named collection of cases.
pub struct Group { pub name: &'static str, pub cases: Vec<Case> }

pub fn group(name: &'static str, cases: Vec<Case>) -> Group { Group { name, cases } }

// ---- ergonomic builders ----
fn base(name: &'static str, bin: Bin) -> Case {
    let engines = match &bin {
        Bin::Source(_) => vec![Engine::Aarch64], // no x86 cross-compiler -> aarch64 only
        Bin::Fixture(fx) => fx.iter().map(|(e, _)| *e).collect(),
        Bin::InRootfs => vec![Engine::Aarch64], // container rootfs fixtures are aarch64 today
    };
    Case { name, bin, args: vec![], rootfs: None, lowers: vec![], mem_max: 0, engines, checks: vec![] }
}
/// A case whose guest is compiled from a C source under `guests/` (aarch64).
pub fn src(name: &'static str, source: &'static str) -> Case { base(name, Bin::Source(source)) }
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
}

/// Result of running one case on one engine.
pub enum Status { Pass, Fail(String), Skip(String) }

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

/// Compile a guest C source for aarch64 (native gcc -static-pie). Cached by mtime. Returns the path.
fn compile_aarch64(ctx: &Ctx, source: &str) -> Result<String, String> {
    let src = ctx.guests.join(source);
    let out = ctx.cache.join("aarch64").join(source.trim_end_matches(".c"));
    let needs = !out.exists()
        || std::fs::metadata(&src).and_then(|m| m.modified()).ok()
            >= std::fs::metadata(&out).and_then(|m| m.modified()).ok();
    if needs {
        let o = Command::new("gcc")
            .args(["-O2", "-static-pie", "-pthread", "-o"])
            .arg(&out).arg(&src).output()
            .map_err(|e| format!("gcc spawn: {e}"))?;
        if !o.status.success() { return Err(format!("compile {source}: {}", String::from_utf8_lossy(&o.stderr).trim())); }
    }
    Ok(out.to_string_lossy().into_owned())
}

/// Provision the guest binary path for a case on an engine. `Ok(None)` = skip (no guest for this arch).
fn provision(ctx: &Ctx, c: &Case, e: Engine) -> Result<Option<String>, String> {
    match &c.bin {
        Bin::Source(s) if e == Engine::Aarch64 => compile_aarch64(ctx, s).map(Some),
        Bin::Source(_) => Ok(None),
        Bin::Fixture(fx) => Ok(fx.iter().find(|(fe, _)| *fe == e).map(|(_, p)| resolve(ctx, p))),
        Bin::InRootfs => Ok(Some(String::new())), // nothing to build; argv[0] is in-rootfs
    }
}
fn resolve(ctx: &Ctx, p: &str) -> String {
    if p.starts_with('/') { p.into() } else { ctx.repo.parent().unwrap().join("poc").join(p).to_string_lossy().into_owned() }
}

/// Run one case on one engine and evaluate its checks.
pub fn run(ctx: &Ctx, c: &Case, e: Engine) -> Status {
    if !c.engines.contains(&e) { return Status::Skip("n/a for engine".into()); }
    if !e.available() { return Status::Skip(format!("{} JIT not built", e.arch())); }
    let guest = match provision(ctx, c, e) {
        Ok(Some(g)) => g,
        Ok(None) => return Status::Skip(format!("no {} guest", e.arch())),
        Err(err) => return Status::Fail(err),
    };
    let rootfs = c.rootfs.and_then(|r| ctx.rootfs_path(r, e));
    if c.rootfs.is_some() && rootfs.is_none() { return Status::Skip(format!("no {} rootfs", e.arch())); }

    let mut cfg = ddjit::SpawnConfig::new(String::new(), rootfs.unwrap_or_default());
    cfg.lowers = c.lowers.clone();
    cfg.mem_max = c.mem_max;
    cfg.argv = match &c.bin {
        Bin::InRootfs => c.args.clone(),
        _ => std::iter::once(guest.clone()).chain(c.args.iter().cloned()).collect(),
    };
    let (prog, args) = match cfg.command(e.jit()) { Some(x) => x, None => return Status::Skip("no command".into()) };
    let out = match Command::new(&prog).args(&args).output() { Ok(o) => o, Err(err) => return Status::Fail(format!("spawn: {err}")) };

    let stdout = strip_noise(&out.stdout);
    let code = out.status.code().unwrap_or(-1);
    for chk in &c.checks {
        if let Err(msg) = eval(chk, &stdout, code, &guest, &c.args) { return Status::Fail(msg); }
    }
    Status::Pass
}

fn eval(chk: &Check, stdout: &str, code: i32, guest: &str, args: &[String]) -> Result<(), String> {
    match chk {
        Check::Exit(want) => (code == *want).then_some(()).ok_or_else(|| format!("exit {code} != {want}")),
        Check::Out(want) => (stdout == *want).then_some(()).ok_or_else(|| format!("stdout {:?} != {:?}", stdout, want)),
        Check::OutHas(sub) => stdout.contains(sub).then_some(()).ok_or_else(|| format!("stdout {:?} lacks {:?}", stdout, sub)),
        Check::Oracle => {
            // native run of the same aarch64 guest on the host = ground truth.
            let o = Command::new(guest).args(args).output().map_err(|e| format!("oracle spawn: {e}"))?;
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

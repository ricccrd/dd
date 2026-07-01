//! Real-software scenario harness — the SECOND test surface, in Rust (no bash).
//!
//! Where `cases` runs compiled guests in-process through the JIT, `scenario` drives **real, popular
//! software** (postgres, redis, node, gcc, distros, …) through a container engine exactly as a developer
//! would. The container daemon is the *vehicle*; the dd **JIT engine is what's under test**.
//!
//! TWO BACKENDS (the key to fast, unblocked authoring):
//!   * [`Backend::Real`] — the host's real `docker`. The **oracle / ground truth**: every scenario must
//!     pass here, which proves the *test* is correct (deterministic, right marker). Authors verify here.
//!   * [`Backend::Dd`]   — `dd-daemon` (the system under test), driven via the `mac` bridge on a linux
//!     dev host (the daemon is a Mach-O binary; env is inline, socket/state under a `/Users` shared
//!     path) or directly on a real macOS host. Divergences from the oracle are dd bugs → `xfail` + GAPS.
//!
//! A [`Scenario`] is one image + how to drive it + what to expect, on each [`Target`] (linux/arm64,
//! linux/amd64; mac lighter-touch). See docs/CHARTER.md and docs/TESTING.md.
//!
//! ```ignore
//! scen("databases/redis-ping", "redis:alpine")
//!     .exec("redis-server --save '' --daemonize yes; sleep 1; redis-cli ping")  // exec -i /bin/sh path
//!     .has("PONG")
//!     .xfail(&[Target::ArmLinux])    // known dd fork+exec gap — passes on Real, xfail on Dd (GAPS.md)
//! ```

use std::collections::HashMap;
use std::path::PathBuf;
use std::process::Command;
use std::sync::{Mutex, OnceLock};
use std::time::{Duration, Instant};

/// Which container engine the scenario runs against.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Backend { Real, Dd }

/// A real-software target. Linux targets map to a docker `--platform`; mac is the lighter native path.
/// Linux parity = a scenario runs on BOTH ArmLinux and AmdLinux unless narrowed.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Target { ArmLinux, AmdLinux, ArmMac }

impl Target {
    pub const LINUX: [Target; 2] = [Target::ArmLinux, Target::AmdLinux];
    pub fn platform(self) -> Option<&'static str> {
        match self { Target::ArmLinux => Some("linux/arm64"), Target::AmdLinux => Some("linux/amd64"), Target::ArmMac => None }
    }
    pub fn label(self) -> &'static str {
        match self { Target::ArmLinux => "arm-linux", Target::AmdLinux => "amd-linux", Target::ArmMac => "arm-mac" }
    }
}

/// Resource class. `Quick` = cache-only/offline-skip, for dev. `Long` = pulls + heavy workloads.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Class { Quick, Long }

/// How the workload is launched in the container.
pub enum Step {
    /// `docker run --rm [--platform p] <image> <argv…>` — one-shot.
    Run(Vec<String>),
    /// Developer-at-a-shell path: start a detached idle container and `docker exec -i <c> /bin/sh -c
    /// <script>` into it (the `exec -it /bin/bash` workflow).
    ExecIt(String),
}

/// One expectation against captured stdout+stderr / exit code.
pub enum Check { Has(String), Eq(String), Rc(i32) }

/// One real-software test.
pub struct Scenario {
    pub id: &'static str,        // "category/name" — stable; xfail + count key on it
    pub image: &'static str,
    pub step: Step,
    pub targets: Vec<Target>,
    pub class: Class,
    pub checks: Vec<Check>,
    pub xfail: Vec<Target>,      // targets where dd is known-broken (still must pass on Real)
    pub timeout: u64,
    pub tty: bool,              // allocate a container PTY (docker -t) → isatty/termios/job-control path
}

pub struct ScenGroup { pub name: &'static str, pub scenarios: Vec<Scenario> }
pub fn sgroup(name: &'static str, scenarios: Vec<Scenario>) -> ScenGroup { ScenGroup { name, scenarios } }

/// Start a scenario: BOTH Linux arches, Quick class by default.
pub fn scen(id: &'static str, image: &'static str) -> Scenario {
    Scenario { id, image, step: Step::Run(vec![]), targets: Target::LINUX.to_vec(),
               class: Class::Quick, checks: vec![], xfail: vec![], timeout: 120, tty: false }
}

impl Scenario {
    pub fn run(mut self, argv: &[&str]) -> Self { self.step = Step::Run(argv.iter().map(|s| s.to_string()).collect()); self }
    pub fn exec(mut self, script: &str) -> Self { self.step = Step::ExecIt(script.to_string()); self }
    pub fn has(mut self, s: &str) -> Self { self.checks.push(Check::Has(s.into())); self }
    pub fn eq_(mut self, s: &str) -> Self { self.checks.push(Check::Eq(s.into())); self }
    pub fn rc(mut self, c: i32) -> Self { self.checks.push(Check::Rc(c)); self }
    pub fn long(mut self) -> Self { self.class = Class::Long; self }
    pub fn only(mut self, t: &[Target]) -> Self { self.targets = t.to_vec(); self }
    pub fn plus_mac(mut self) -> Self { if !self.targets.contains(&Target::ArmMac) { self.targets.push(Target::ArmMac); } self }
    pub fn xfail(mut self, t: &[Target]) -> Self { self.xfail = t.to_vec(); self }
    pub fn timeout(mut self, s: u64) -> Self { self.timeout = s; self }
    /// Allocate a container PTY (`docker run/exec -t`) so the guest sees an interactive TERMINAL —
    /// isatty()==1, termios tcgetattr/tcsetattr, ioctl(TIOCGWINSZ), and job-control signals. The
    /// developer `docker exec -it /bin/bash` path; exercises the JIT's pty/termios/ioctl syscalls.
    pub fn tty(mut self) -> Self { self.tty = true; self }
}

/// Verdict for one (scenario, target).
pub enum Status { Pass, Fail(String), Skip(String), Xfail(String), Xpass }

/// Runner config.
pub struct Cfg {
    pub backend: Backend,
    pub class: Class,
    pub targets: Vec<Target>,
    pub category: Option<String>,
    pub offline: bool,
    pub count: bool,
    pub images: PathBuf,
    pub daemon_bin: PathBuf,
}
impl Cfg {
    pub fn includes(&self, s: &Scenario) -> bool { self.class == Class::Long || s.class == Class::Quick }
}

// ---- the mac bridge ------------------------------------------------------------------------------
// On a linux dev host, dd-daemon + its docker live mac-side: we run script FILES through `mac bash
// <file>` (env inline in the file, paths under a /Users shared dir). On macOS we run them directly.
// Script files (not `-lc` strings) sidestep all quote-escaping of embedded workloads/heredocs.
fn on_mac_host() -> bool { cfg!(target_os = "macos") }
fn shared_run_dir() -> PathBuf {
    // Must be visible to the mac side → under the repo (/Users/... shared mount), not /tmp.
    let d = PathBuf::from(env!("CARGO_MANIFEST_DIR")).parent().unwrap().join("target/dd-scen");
    std::fs::create_dir_all(&d).ok();
    d
}
/// Run a generated bash script file, optionally bridged to the mac side, under a linux `timeout`.
fn run_script(file: &std::path::Path, bridged: bool, timeout_s: u64) -> std::process::Output {
    let mut c = Command::new("timeout");
    c.arg(timeout_s.to_string());
    if bridged && !on_mac_host() { c.arg("mac").arg("bash").arg(file); } else { c.arg("bash").arg(file); }
    c.output().unwrap_or_else(|e| panic!("run_script {}: {e}", file.display()))
}
/// Spawn a long-lived script (the daemon), bridged on linux. Returns the child to kill on teardown.
fn spawn_script(file: &std::path::Path, bridged: bool) -> std::io::Result<std::process::Child> {
    if bridged && !on_mac_host() { Command::new("mac").arg("bash").arg(file).spawn() }
    else { Command::new("bash").arg(file).spawn() }
}
fn sh_quote(s: &str) -> String { format!("'{}'", s.replace('\'', "'\\''")) }

// ---- speed: caches + per-cell isolation ----------------------------------------------------------
// The runner used to pay a `mac`-bridge round-trip for EVERY scenario×target just to re-confirm the
// image is present, and re-ran the (deterministic) Real-docker oracle every time. Both verdicts are
// invariant for a whole run, so we memoize them. Combined with the worker pool in `scenarios.rs` this
// turns the lane from one serial bridge call per cell into a handful of cached, parallel ones.

/// `image → availability` verdict, computed once per image instead of once per scenario×target.
fn ensure_cache() -> &'static Mutex<HashMap<String, bool>> {
    static C: OnceLock<Mutex<HashMap<String, bool>>> = OnceLock::new();
    C.get_or_init(|| Mutex::new(HashMap::new()))
}
/// Real-backend (oracle) output cache, keyed by the logical run `(image,step,target,tty)`. The oracle
/// is ground-truth-deterministic, so an identical cell never needs to hit the bridge twice.
fn oracle_cache() -> &'static Mutex<HashMap<String, (String, i32)>> {
    static C: OnceLock<Mutex<HashMap<String, (String, i32)>>> = OnceLock::new();
    C.get_or_init(|| Mutex::new(HashMap::new()))
}
/// The logical key that fully determines a cell's output (no pid/host noise → stable across cells).
fn cell_key(s: &Scenario, t: Target) -> String {
    let step = match &s.step { Step::Run(a) => format!("run\u{1}{}", a.join("\u{1}")), Step::ExecIt(x) => format!("exec\u{1}{x}") };
    format!("{}\u{1}{}\u{1}{}\u{1}{}", s.image, step, t.label(), s.tty)
}
/// Make a string safe to embed in a filename (image refs carry `/`, `:`, `@`).
fn slug(s: &str) -> String { s.chars().map(|c| if c.is_ascii_alphanumeric() { c } else { '_' }).collect() }
/// Phase timing is opt-in (`DD_SCEN_PROFILE=1`) so it never pollutes normal output.
fn profiling() -> bool { std::env::var_os("DD_SCEN_PROFILE").is_some() }

// ---- the daemon (Dd backend only) ----------------------------------------------------------------
pub struct Daemon { child: Option<std::process::Child>, dir: PathBuf, log: PathBuf, host: String, bridged: bool }

impl Daemon {
    /// Last `n` lines of the daemon log — where engine diagnostics (`unhandled syscall`, `[jit86]
    /// UNIMPL`, CRASHDBG rip dumps) actually print. Empty for the Real backend.
    pub fn log_tail(&self, n: usize) -> String {
        if self.log.as_os_str().is_empty() { return String::new(); }
        std::fs::read_to_string(&self.log).map(|s| {
            let lines: Vec<&str> = s.lines().collect();
            lines[lines.len().saturating_sub(n)..].join("\n")
        }).unwrap_or_default()
    }
}

impl Daemon {
    /// Real backend → no daemon to manage (host docker is already up). Dd backend → boot dd-daemon
    /// (bridged on linux) on a private socket/state under the shared run dir.
    pub fn boot(cfg: &Cfg) -> Result<Daemon, String> {
        let bridged = !on_mac_host();
        if cfg.backend == Backend::Real {
            // Real oracle = the host's Docker Desktop, reached through the SAME `mac` bridge but with the
            // DEFAULT docker context (no DOCKER_HOST). No daemon to manage; we just need a script dir.
            let dir = shared_run_dir().join(format!("real-{}", std::process::id()));
            std::fs::create_dir_all(&dir).ok();
            return Ok(Daemon { child: None, dir, log: PathBuf::new(), host: String::new(), bridged });
        }
        let dir = shared_run_dir().join(format!("dd-{}", std::process::id()));
        std::fs::create_dir_all(&dir).map_err(|e| e.to_string())?;
        let sock = dir.join("dd.sock");
        let log = dir.join("daemon.log");
        let _ = std::fs::remove_file(&sock);
        // Start a fresh daemon on a PRIVATE socket/state. NO global pkill — many daemons run in
        // parallel (one engine per worker), so we record THIS daemon's pid and kill only it on teardown.
        let boot_sh = dir.join("boot.sh");
        std::fs::write(&boot_sh, format!(
            "echo $$ > {dir}/daemon.pid\nexport DD_IMAGES={img}\nexport DDOCKERD_SOCK={sock}\n\
             export DD_STATE={state}\nexport DD_VOLUMES={vol}\nexec {bin} > {log} 2>&1\n",
            dir = sh_quote(&dir.to_string_lossy()),
            img = sh_quote(&cfg.images.to_string_lossy()), sock = sh_quote(&sock.to_string_lossy()),
            state = sh_quote(&dir.join("state.json").to_string_lossy()), vol = sh_quote(&dir.join("vol").to_string_lossy()),
            bin = sh_quote(&cfg.daemon_bin.to_string_lossy()), log = sh_quote(&log.to_string_lossy()),
        )).map_err(|e| e.to_string())?;
        let child = spawn_script(&boot_sh, bridged).map_err(|e| format!("spawn daemon: {e}"))?;
        for _ in 0..160 { if sock.exists() { break; } std::thread::sleep(Duration::from_millis(250)); }
        if !sock.exists() {
            let tail = std::fs::read_to_string(&log).unwrap_or_default();
            return Err(format!("dd-daemon failed to start; log tail:\n{}", tail.lines().rev().take(15).collect::<Vec<_>>().join("\n")));
        }
        Ok(Daemon { child: Some(child), dir, log: log.clone(), host: format!("unix://{}", sock.display()), bridged })
    }
    fn docker_host(&self) -> Option<&str> { if self.host.is_empty() { None } else { Some(&self.host) } }
}

impl Drop for Daemon {
    fn drop(&mut self) {
        if let Some(c) = self.child.as_mut() { let _ = c.kill(); }
        if self.bridged && !self.dir.as_os_str().is_empty() {
            // reap ONLY this worker's mac-side daemon (by recorded pid) — never a sibling's.
            let k = self.dir.join("kill.sh");
            let body = format!("p=$(cat {}/daemon.pid 2>/dev/null); [ -n \"$p\" ] && kill \"$p\" 2>/dev/null; true\n",
                sh_quote(&self.dir.to_string_lossy()));
            if std::fs::write(&k, body).is_ok() { let _ = run_script(&k, true, 15); }
        }
        if !self.dir.as_os_str().is_empty() { let _ = std::fs::remove_dir_all(&self.dir); }
    }
}

// ---- generated per-operation scripts -------------------------------------------------------------
fn header(host: Option<&str>) -> String {
    match host { Some(h) => format!("#!/bin/bash\nexport DOCKER_HOST={}\n", sh_quote(h)), None => "#!/bin/bash\n".into() }
}
fn ensure(d: &Daemon, cfg: &Cfg, image: &str) -> bool {
    // Image availability is invariant for the whole run → memoize per image. This is the single biggest
    // bridge-call saver: a category that reuses one image across N scenarios used to inspect it N times.
    if let Some(&ok) = ensure_cache().lock().unwrap().get(image) { return ok; }
    let dir = if d.dir.as_os_str().is_empty() { shared_run_dir() } else { d.dir.clone() };
    // Per-image filename so concurrent first-touches of DIFFERENT images don't clobber one script.
    let f = dir.join(format!("ensure-{}.sh", slug(image)));
    let body = format!("{}docker image inspect {img} >/dev/null 2>&1 && exit 0\n{}\ndocker pull {img} >/dev/null 2>&1\n",
        header(d.docker_host()), if cfg.offline { "exit 1" } else { "" }, img = sh_quote(image));
    if std::fs::write(&f, body).is_err() { return false; }
    // Run unlocked (a pull can take minutes); two threads racing the SAME image just inspect twice — the
    // op is idempotent. Record the verdict so every later cell using this image is a pure cache hit.
    let ok = run_script(&f, d.bridged, if cfg.offline { 20 } else { 180 }).status.success();
    ensure_cache().lock().unwrap().insert(image.to_string(), ok);
    ok
}

fn drive(d: &Daemon, s: &Scenario, t: Target, cfg: &Cfg) -> (String, i32) {
    // Oracle output is deterministic ground truth → serve repeats of an identical cell from cache.
    let key = (cfg.backend == Backend::Real).then(|| cell_key(s, t));
    if let Some(k) = &key {
        if let Some(v) = oracle_cache().lock().unwrap().get(k) { return v.clone(); }
    }
    let dir = if d.dir.as_os_str().is_empty() { shared_run_dir() } else { d.dir.clone() };
    // Per-(scenario,target) filename so the two arches of one scenario can run concurrently without
    // racing on a shared op script.
    let f = dir.join(format!("op-{}-{}.sh", s.id.replace('/', "_"), t.label()));
    let plat = t.platform().map(|p| format!("--platform {p} ")).unwrap_or_default();
    let tt = if s.tty { "-t " } else { "" };          // run: allocate a container PTY
    let xt = if s.tty { "-t" } else { "-i" };          // exec: PTY vs plain stdin (no client TTY needed)
    let sh = if s.image.contains("alpine") || s.image.starts_with("busybox") { "/bin/sh" } else { "/bin/bash" };
    let body = match &s.step {
        Step::Run(argv) => {
            let a = argv.iter().map(|x| sh_quote(x)).collect::<Vec<_>>().join(" ");
            format!("{}docker run --rm {tt}{plat}{img} {a}\n", header(d.docker_host()), img = sh_quote(s.image))
        }
        Step::ExecIt(script) => {
            // idle container we exec into (mirrors `docker exec -it`); fall back to one-shot run for
            // images with no keep-alive shell (distroless). Embed the user script verbatim via a
            // quoted heredoc so arbitrary quotes/heredocs inside it survive.
            let name = format!("ddx-{}-{}-{}", std::process::id(), s.id.replace('/', "-"), t.label());
            format!(
"{hdr}N={name}
docker rm -f $N >/dev/null 2>&1
if docker run -d --name $N {plat}{img} {sh} -c 'while true; do sleep 3600; done' >/dev/null 2>&1; then
  docker exec {xt} $N {sh} -c \"$(cat <<'DDEOF'
{script}
DDEOF
)\"
  rc=$?
  docker rm -f $N >/dev/null 2>&1
else
  docker run --rm {tt}{plat}{img} {sh} -c \"$(cat <<'DDEOF'
{script}
DDEOF
)\"
  rc=$?
fi
exit $rc
", hdr = header(d.docker_host()), name = sh_quote(&name), plat = plat, tt = tt, xt = xt, img = sh_quote(s.image), sh = sh, script = script)
        }
    };
    if std::fs::write(&f, body).is_err() { return ("(failed to write op script)".into(), -1); }
    let o = run_script(&f, d.bridged, s.timeout + 10);
    let mut out = String::from_utf8_lossy(&o.stdout).into_owned();
    out.push_str(&String::from_utf8_lossy(&o.stderr));
    let res = (out, o.status.code().unwrap_or(-1));
    if let Some(k) = key { oracle_cache().lock().unwrap().insert(k, res.clone()); }
    res
}

/// Run one scenario on one target and classify (xfail-aware). xfail only applies to the Dd backend —
/// on Real, a fail is always a real (test) failure.
pub fn run_one(d: &Daemon, s: &Scenario, t: Target, cfg: &Cfg) -> Status {
    if !s.targets.contains(&t) { return Status::Skip("n/a for target".into()); }
    let prof = profiling();
    let t0 = Instant::now();
    let cached = ensure_cache().lock().unwrap().contains_key(s.image);
    if !ensure(d, cfg, s.image) { return Status::Skip(format!("image {} unavailable", s.image)); }
    let ensure_ms = t0.elapsed().as_millis();
    let xfail = cfg.backend == Backend::Dd && s.xfail.contains(&t);
    let t1 = Instant::now();
    let (out, code) = drive(d, s, t, cfg);
    if prof {
        eprintln!("[prof] id={} tgt={} ensure_ms={} ensure_cached={} drive_ms={} total_ms={}",
            s.id, t.label(), ensure_ms, cached as u8, t1.elapsed().as_millis(), t0.elapsed().as_millis());
    }
    let bad: Option<String> = if code == 124 { Some(format!("timeout >{}s", s.timeout)) } else {
        s.checks.iter().find_map(|chk| match chk {
            Check::Has(sub) => (!out.contains(sub.as_str())).then(|| format!("lacks [{sub}] in [{}]", clip(&out))),
            Check::Eq(want) => (out.trim() != want.as_str()).then(|| format!("got [{}] want [{want}]", clip(out.trim()))),
            Check::Rc(want) => (code != *want).then(|| format!("rc {code} != {want}")),
        })
    };
    match (bad, xfail) {
        (None, true) => Status::Xpass,
        (None, false) => Status::Pass,
        (Some(m), xf) => {
            // Self-explaining failure: scrape the container output + daemon-log tail for engine signals
            // (missing syscall / UNIMPL opcode / crash / loader) and attach a one-line diagnosis.
            let diag = crate::diag::diagnose(m, code, &out, &d.log_tail(25));
            if xf { Status::Xfail(diag.summary()) } else { Status::Fail(diag.summary()) }
        }
    }
}

fn clip(s: &str) -> String { s.replace('\n', "|").chars().take(180).collect() }

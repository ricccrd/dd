//! dd benchmark — the SAME Linux binary timed two ways:
//!   VM   = run it inside the Linux VM (aarch64 native; x86_64 under qemu-user) — the VM-based-Docker path
//!   dd   = run it through dd's JIT on the macOS host, no VM (via the `mac` bridge)
//! Median-of-N wall time; both lanes' stdout is compared so we only time equivalent work.
//!
//!   cargo run -q -p dd-tests --release --bin bench
//!
//! Honest notes baked into the output:
//!  - the dd lane here pays a small `mac`-bridge spawn tax the real macOS app does not → conservative.
//!  - aarch64 VM = native speed (best case for a VM); x86_64 VM = qemu-user emulation.

use ddjit::{Guest, SpawnConfig};
use std::path::{Path, PathBuf};
use std::process::Command;
use std::time::Instant;

const SQLITE_URL: &str = "https://www.sqlite.org/2024/sqlite-amalgamation-3450000.zip";
const REPS: usize = 7;

struct Work { name: &'static str, srcs: &'static [&'static str], libs: &'static [&'static str], defs: &'static [&'static str], sqlite: bool }

fn works() -> Vec<Work> {
    vec![
        Work { name: "int-sieve",  srcs: &["b_int.c"],   libs: &[],     defs: &[], sqlite: false },
        Work { name: "float-nbody",srcs: &["b_float.c"], libs: &["-lm"],defs: &[], sqlite: false },
        Work { name: "sha256",     srcs: &["b_hash.c"],  libs: &[],     defs: &[], sqlite: false },
        Work { name: "memcpy",     srcs: &["b_memcpy.c"],libs: &[],     defs: &[], sqlite: false },
        Work { name: "matmul",     srcs: &["b_matmul.c"],libs: &[],     defs: &[], sqlite: false },
        Work { name: "mandelbrot", srcs: &["b_mandel.c"],libs: &[],     defs: &[], sqlite: false },
        Work { name: "qsort",      srcs: &["b_sort.c"],  libs: &[],     defs: &[], sqlite: false },
        Work { name: "base64",     srcs: &["b_base64.c"],libs: &[],     defs: &[], sqlite: false },
        Work { name: "text-scan",  srcs: &["b_strings.c"],libs: &[],    defs: &[], sqlite: false },
        Work { name: "sqlite",     srcs: &["b_sqlite.c"],libs: &["-lm"],
               defs: &["-DSQLITE_THREADSAFE=0","-DSQLITE_OMIT_LOAD_EXTENSION","-DSQLITE_DEFAULT_MEMSTATUS=0"], sqlite: true },
    ]
}

fn sh(cmd: &str, args: &[&str]) -> Result<String, String> {
    let o = Command::new(cmd).args(args).output().map_err(|e| format!("{cmd}: {e}"))?;
    if !o.status.success() { return Err(format!("{cmd} {:?}: {}", args, String::from_utf8_lossy(&o.stderr).trim().to_string())); }
    Ok(String::from_utf8_lossy(&o.stdout).into_owned())
}

/// Fetch + unpack the SQLite amalgamation into `cache` (once). Returns the dir holding sqlite3.c/.h.
fn ensure_sqlite(cache: &Path) -> Result<PathBuf, String> {
    let dir = cache.join("sqlite");
    if dir.join("sqlite3.c").exists() { return Ok(dir); }
    std::fs::create_dir_all(&dir).ok();
    let zip = cache.join("sqlite.zip");
    eprintln!("[bench] fetching SQLite amalgamation…");
    sh("curl", &["-sL", "-o", zip.to_str().unwrap(), SQLITE_URL])?;
    // unzip (fallback to python if `unzip` is absent)
    if sh("unzip", &["-oq", zip.to_str().unwrap(), "-d", dir.to_str().unwrap()]).is_err() {
        sh("python3", &["-c", &format!("import zipfile;zipfile.ZipFile(r'{}').extractall(r'{}')", zip.display(), dir.display())])?;
    }
    // the zip nests a sqlite-amalgamation-*/ dir; lift sqlite3.c/.h up
    for entry in std::fs::read_dir(&dir).map_err(|e| e.to_string())? {
        let p = entry.map_err(|e| e.to_string())?.path();
        if p.is_dir() {
            for f in ["sqlite3.c", "sqlite3.h"] {
                if p.join(f).exists() { std::fs::copy(p.join(f), dir.join(f)).ok(); }
            }
        }
    }
    if !dir.join("sqlite3.c").exists() { return Err("sqlite3.c not found after unzip".into()); }
    Ok(dir)
}

fn cc_for(arch: &str) -> &'static str { if arch == "x86_64" { "x86_64-linux-gnu-gcc" } else { "gcc" } }

/// Compile a workload for one arch (static-PIE, -O2), cached. Returns the binary path.
fn compile(w: &Work, arch: &str, guests: &Path, cache: &Path, sqlite_dir: &Path) -> Result<PathBuf, String> {
    let out = cache.join(arch).join(w.name);
    std::fs::create_dir_all(out.parent().unwrap()).ok();
    if out.exists() { return Ok(out); }
    let mut args: Vec<String> = vec!["-O2".into(), "-static-pie".into(), "-pthread".into(), "-o".into(), out.to_string_lossy().into()];
    for d in w.defs { args.push((*d).into()); }
    if w.sqlite { args.push(format!("-I{}", sqlite_dir.display())); }
    for s in w.srcs { args.push(guests.join(s).to_string_lossy().into()); }
    if w.sqlite { args.push(sqlite_dir.join("sqlite3.c").to_string_lossy().into()); }
    for l in w.libs { args.push((*l).into()); }
    let aref: Vec<&str> = args.iter().map(|s| s.as_str()).collect();
    sh(cc_for(arch), &aref)?;
    Ok(out)
}

/// Median-of-REPS wall seconds for a command (one warmup discarded). Returns (median_s, stdout).
fn timed(prog: &str, args: &[String]) -> Result<(f64, String), String> {
    // warmup (also captures reference stdout)
    let o = Command::new(prog).args(args).output().map_err(|e| format!("{prog}: {e}"))?;
    if !o.status.success() && o.status.code() != Some(0) {
        return Err(format!("{prog} exited {:?}: {}", o.status.code(), String::from_utf8_lossy(&o.stderr).trim()));
    }
    let stdout = strip_noise(&o.stdout);
    let mut ts = Vec::with_capacity(REPS);
    for _ in 0..REPS {
        let t = Instant::now();
        let _ = Command::new(prog).args(args).output().map_err(|e| format!("{prog}: {e}"))?;
        ts.push(t.elapsed().as_secs_f64());
    }
    ts.sort_by(|a, b| a.partial_cmp(b).unwrap());
    Ok((ts[REPS / 2], stdout))
}

fn strip_noise(b: &[u8]) -> String {
    String::from_utf8_lossy(b).lines().filter(|l| !l.contains("unhandled syscall")).collect::<Vec<_>>().join("\n")
}

fn main() {
    let manifest = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
    let guests = manifest.join("guests/bench");
    let cache = manifest.join(".bench-cache");
    std::fs::create_dir_all(&cache).ok();

    let sqlite_dir = match ensure_sqlite(&cache) { Ok(d) => d, Err(e) => { eprintln!("[bench] sqlite unavailable: {e}"); cache.join("sqlite") } };

    let arches = [("aarch64", Guest::LinuxAarch64), ("x86_64", Guest::LinuxX86_64)];
    println!("\n  workload      arch      VM (s)     dd JIT (s)   dd / VM");
    println!("  {}", "─".repeat(60));

    let mut rows: Vec<(String, String, f64, f64, String)> = Vec::new();
    for w in works() {
        for (arch, guest) in arches {
            if w.sqlite && !sqlite_dir.join("sqlite3.c").exists() { continue; }
            let bin = match compile(&w, arch, &guests, &cache, &sqlite_dir) {
                Ok(b) => b, Err(e) => { eprintln!("[bench] compile {}/{arch}: {e}", w.name); continue; }
            };
            let binstr = bin.to_string_lossy().into_owned();

            // VM lane: aarch64 native, x86_64 under qemu-user.
            let vm = if arch == "x86_64" {
                timed("qemu-x86_64", &[binstr.clone()])
            } else {
                timed(&binstr, &[])
            };
            // dd lane: through the JIT on the macOS host.
            let mut cfg = SpawnConfig::new(String::new(), String::new());
            cfg.argv = vec![binstr.clone()];
            let jit = match cfg.command(guest) {
                Some((prog, a)) => timed(&prog, &a),
                None => { eprintln!("[bench] {} JIT not built", arch); continue; }
            };
            match (vm, jit) {
                (Ok((vt, vo)), Ok((jt, jo))) => {
                    let ok = if vo.trim() == jo.trim() { "" } else { "  ⚠ output differs" };
                    let ratio = format!("{:.2}×{}", jt / vt, ok);
                    println!("  {:<13} {:<8} {:>7.3}     {:>7.3}      {:>6}", w.name, arch, vt, jt, ratio);
                    rows.push((w.name.into(), arch.into(), vt, jt, ratio));
                }
                (Err(e), _) | (_, Err(e)) => eprintln!("[bench] {}/{arch}: {e}", w.name),
            }
        }
    }

    // emit a markdown table for pasting into docs
    println!("\n--- markdown ---\n");
    println!("| Workload | Arch | VM (s) | dd JIT (s) | dd / VM |");
    println!("|---|---|--:|--:|--:|");
    for (n, a, vt, jt, r) in &rows {
        println!("| {n} | {a} | {vt:.3} | {jt:.3} | {} |", r.replace(" ⚠ output differs", " ⚠"));
    }
}

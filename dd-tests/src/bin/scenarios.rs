//! Real-software scenario runner — drives popular images through a container engine (the second test
//! surface; the first is `dd-tests`, the in-process basics matrix). Rust, no bash.
//!
//!   cargo run -p dd-tests --bin scenarios -- --backend real        # host docker = ORACLE (prove tests)
//!   cargo run -p dd-tests --bin scenarios -- --backend dd          # dd-daemon = SYSTEM UNDER TEST
//!   cargo run -p dd-tests --bin scenarios -- --backend dd --long   # full compatibility sweep (pulls)
//!   cargo run -p dd-tests --bin scenarios -- -c databases -t arm   # one category, one arch
//!   cargo run -p dd-tests --bin scenarios -- --count               # list every case + total, run nothing
//!
//! Each invocation boots its OWN engine/daemon (private socket) so many runners go in parallel.

use dd_tests::scenario::{run_one, Backend, Cfg, Class, Daemon, Scenario, Status, Step, Target};
use dd_tests::scenarios;
use std::path::PathBuf;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::Mutex;
use std::time::Instant;

/// A cheap a-priori estimate of how long a cell takes, used to schedule longest-first (LPT): kicking the
/// multi-second builds/server-boots off first keeps the pool from ending on a long tail. Heavy = an
/// `exec` workload that compiles or installs a toolchain / boots a server (the real long poles); a
/// plain one-shot `run` is light. `Long`-class cases dominate either way.
fn cell_weight(s: &Scenario) -> usize {
    const HEAVY: &[&str] = &["apk add", "apt-get", "apt ", "dnf ", "yum ", "cmake", "make", "build-base",
        "gcc", "g++", "clang", "go build", "go run", "rustc", "cargo ", "dotnet", "npm", "pip ",
        "docker-entrypoint", "mongod", "mysqld", "mariadbd", "postgres", "redis-server", "valkey-server",
        "nginx", "httpd", "caddy", "haproxy", "memcached"];
    let base = match &s.step {
        Step::ExecIt(script) => 1_000 + script.len() + HEAVY.iter().filter(|k| script.contains(*k)).count() * 100_000,
        Step::Run(argv) => argv.iter().map(|x| x.len()).sum(),
    };
    if s.class == Class::Long { base + 1_000_000 } else { base }
}

fn parse_target(s: &str) -> Option<Target> {
    match s { "arm" | "arm-linux" | "arm64" => Some(Target::ArmLinux),
              "amd" | "amd-linux" | "x86_64" | "amd64" => Some(Target::AmdLinux),
              "mac" | "arm-mac" | "darwin" => Some(Target::ArmMac), _ => None }
}

fn main() {
    let argv: Vec<String> = std::env::args().skip(1).collect();
    let (mut backend, mut class, mut category, mut count) = (Backend::Dd, Class::Quick, None, false);
    let mut targets = Target::LINUX.to_vec();
    let mut it = argv.iter();
    while let Some(a) = it.next() {
        match a.as_str() {
            "--backend" => match it.next().map(|s| s.as_str()) {
                Some("real") => backend = Backend::Real,
                Some("dd") => backend = Backend::Dd,
                other => { eprintln!("--backend real|dd (got {other:?})"); std::process::exit(2); } },
            "--long" => class = Class::Long,
            "--quick" => class = Class::Quick,
            "--count" => count = true,
            "-c" | "--category" => category = it.next().cloned(),
            "-t" | "--target" => targets = it.next().and_then(|s| parse_target(s)).map(|t| vec![t]).unwrap_or(targets),
            "-h" | "--help" => { eprintln!("usage: scenarios [--backend real|dd] [--long] [--count] [-c cat] [-t arm|amd|mac]"); return; }
            other => { eprintln!("unknown arg: {other}"); std::process::exit(2); }
        }
    }
    let repo = PathBuf::from(env!("CARGO_MANIFEST_DIR")).parent().unwrap().to_path_buf();
    let cfg = Cfg {
        backend, class, targets, category: category.clone(),
        offline: class == Class::Quick,                 // quick = cache-only
        count,
        images: std::env::var("DD_IMAGES").map(PathBuf::from).unwrap_or_else(|_| PathBuf::from("/Users/x/dd/poc/images")),
        daemon_bin: std::env::var("DD_DAEMON").map(PathBuf::from).unwrap_or_else(|_| repo.join("target/release/dd-daemon")),
    };

    let groups = scenarios::all();
    let matches = |g: &str| category.as_ref().map_or(true, |c| g.contains(c.as_str()));

    if count {
        let mut n = 0;
        for g in &groups { if !matches(g.name) { continue; } println!("{}", g.name);
            for s in &g.scenarios { if !cfg.includes(s) { continue; }
                for t in &cfg.targets { if s.targets.contains(t) { n += 1; println!("  {:<44} [{}]", s.id, t.label()); } } } }
        println!("\n{n} scenario×target cases");
        return;
    }

    let daemon = match Daemon::boot(&cfg) { Ok(d) => d, Err(e) => { eprintln!("{e}"); std::process::exit(2); } };
    eprintln!("scenarios · backend={:?} · class={:?} · targets={:?}",
        cfg.backend, cfg.class, cfg.targets.iter().map(|t| t.label()).collect::<Vec<_>>());

    // Worker pool size: env override, else ≈cores capped at 6 (modest — too many concurrent `mac`-bridge
    // children invite SIGTERM contention). `DD_SCEN_JOBS=1` forces the old fully-serial behaviour.
    let jobs_n = std::env::var("DD_SCEN_JOBS").ok().and_then(|v| v.parse::<usize>().ok()).filter(|&n| n >= 1)
        .unwrap_or_else(|| std::thread::available_parallelism().map(|n| n.get().min(6)).unwrap_or(4));
    eprintln!("            · jobs={jobs_n} (terminal category forced serial)");

    let (mut pass, mut fail, mut xfail, mut xpass, mut skip) = (0u32, 0u32, 0u32, 0u32, 0u32);
    let mut failures: Vec<String> = vec![]; let mut xpasses: Vec<String> = vec![];
    let wall = Instant::now();

    // The displayed groups (in registry order) and their selected scenarios. Output ordering/format is
    // computed from `results` AFTER execution, so scheduling order never changes what's printed.
    let displayed: Vec<(&str, Vec<&Scenario>)> = groups.iter()
        .filter(|g| matches(g.name))
        .map(|g| (g.name, g.scenarios.iter().filter(|s| cfg.includes(s)).collect::<Vec<_>>()))
        .filter(|(_, sel)| !sel.is_empty())
        .collect();
    let nt = cfg.targets.len();
    let mut results: Vec<Vec<Vec<Option<Status>>>> = displayed.iter()
        .map(|(_, sel)| (0..sel.len()).map(|_| (0..nt).map(|_| None).collect()).collect()).collect();

    // `terminal` stays strictly serial (PTY / job-control / controlling-TTY share process-wide state).
    for (gi, (name, sel)) in displayed.iter().enumerate() {
        if !name.contains("terminal") { continue; }
        for (si, s) in sel.iter().enumerate() {
            for (ti, &t) in cfg.targets.iter().enumerate() {
                results[gi][si][ti] = Some(run_one(&daemon, s, t, &cfg));
            }
        }
    }

    // Everything else runs in ONE global pool across ALL groups (no per-group drain barrier → the 20s
    // toolchain builds overlap the rest instead of gating it), scheduled longest-first.
    let mut jobs: Vec<(usize, usize, usize)> = vec![];
    for (gi, (name, sel)) in displayed.iter().enumerate() {
        if name.contains("terminal") { continue; }
        for si in 0..sel.len() { for ti in 0..nt { jobs.push((gi, si, ti)); } }
    }
    jobs.sort_by_key(|&(gi, si, _)| std::cmp::Reverse(cell_weight(displayed[gi].1[si])));
    let slots: Vec<Mutex<Option<Status>>> = jobs.iter().map(|_| Mutex::new(None)).collect();
    if jobs_n <= 1 {
        for (i, &(gi, si, ti)) in jobs.iter().enumerate() {
            *slots[i].lock().unwrap() = Some(run_one(&daemon, displayed[gi].1[si], cfg.targets[ti], &cfg));
        }
    } else {
        let cursor = AtomicUsize::new(0);
        std::thread::scope(|scope| {
            for _ in 0..jobs_n.min(jobs.len().max(1)) {
                scope.spawn(|| loop {
                    let i = cursor.fetch_add(1, Ordering::Relaxed);
                    if i >= jobs.len() { break; }
                    let (gi, si, ti) = jobs[i];
                    let st = run_one(&daemon, displayed[gi].1[si], cfg.targets[ti], &cfg);
                    *slots[i].lock().unwrap() = Some(st);
                });
            }
        });
    }
    for (i, &(gi, si, ti)) in jobs.iter().enumerate() { results[gi][si][ti] = slots[i].lock().unwrap().take(); }

    // Print every group in registry order from the collected verdicts (identical layout to the serial run).
    for (gi, (name, sel)) in displayed.iter().enumerate() {
        println!("\n\x1b[1m{}\x1b[0m", name);
        for (si, s) in sel.iter().enumerate() {
            print!("  {:<44}", s.id);
            for (ti, &t) in cfg.targets.iter().enumerate() {
                match results[gi][si][ti].as_ref().unwrap() {
                    Status::Skip(_) => { skip += 1; print!("  \x1b[90m· {}\x1b[0m", t.label()); }
                    Status::Pass => { pass += 1; print!("  \x1b[32m✓ {}\x1b[0m", t.label()); }
                    Status::Fail(m) => { fail += 1; print!("  \x1b[31m✗ {}\x1b[0m", t.label());
                        failures.push(format!("{} [{}]: {}", s.id, t.label(), m)); }
                    Status::Xfail(_) => { xfail += 1; print!("  \x1b[33mx {}\x1b[0m", t.label()); }
                    Status::Xpass => { xpass += 1; print!("  \x1b[35m✓! {}\x1b[0m", t.label());
                        xpasses.push(format!("{} [{}]", s.id, t.label())); }
                }
            }
            println!();
        }
    }
    println!("\n{}", "─".repeat(60));
    for f in &failures { println!("\x1b[31m✗\x1b[0m {f}"); }
    for x in &xpasses { println!("\x1b[35m✓!\x1b[0m {x} — XPASS (gap fixed? drop the .xfail marker)"); }
    let color = if fail > 0 { "31" } else { "32" };
    println!("\x1b[1;{color}m{pass} passed\x1b[0m  {fail} failed  \x1b[33m{xfail} xfail\x1b[0m  \x1b[35m{xpass} xpass\x1b[0m  \x1b[90m{skip} skip   {}ms\x1b[0m",
        wall.elapsed().as_millis());
    std::process::exit(if fail > 0 { 1 } else { 0 });
}

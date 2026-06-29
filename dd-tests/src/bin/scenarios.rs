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

use dd_tests::scenario::{run_one, Backend, Cfg, Class, Daemon, Status, Target};
use dd_tests::scenarios;
use std::path::PathBuf;
use std::time::Instant;

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

    let (mut pass, mut fail, mut xfail, mut xpass, mut skip) = (0u32, 0u32, 0u32, 0u32, 0u32);
    let mut failures: Vec<String> = vec![]; let mut xpasses: Vec<String> = vec![];
    let wall = Instant::now();
    for g in &groups {
        if !matches(g.name) { continue; }
        let sel: Vec<_> = g.scenarios.iter().filter(|s| cfg.includes(s)).collect();
        if sel.is_empty() { continue; }
        println!("\n\x1b[1m{}\x1b[0m", g.name);
        for s in sel {
            print!("  {:<44}", s.id);
            for &t in &cfg.targets {
                match run_one(&daemon, s, t, &cfg) {
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

//! dd-tests runner — runs the engine × case matrix and prints a grouped report.
//!
//!   cargo run -p dd-tests                 # everything, every engine
//!   cargo run -p dd-tests -- container    # only groups/cases matching "container"
//!   cargo run -p dd-tests -- -e x86_64    # only the x86-64 engine
//!   cargo run -p dd-tests -- --list       # list groups + cases without running

use dd_tests::{cases, run, Ctx, Engine, Status};

fn parse_engine(s: &str) -> Option<Engine> {
    match s { "aarch64" | "arm64" => Some(Engine::Aarch64), "x86_64" | "amd64" => Some(Engine::X86_64), _ => None }
}

fn main() {
    let argv: Vec<String> = std::env::args().skip(1).collect();
    let (mut engine_filter, mut name_filter, mut list) = (None, None, false);
    let mut it = argv.iter();
    while let Some(a) = it.next() {
        match a.as_str() {
            "-e" | "--engine" => engine_filter = it.next().and_then(|s| parse_engine(s)),
            "--list" => list = true,
            "-h" | "--help" => { eprintln!("usage: dd-tests [--engine aarch64|x86_64] [--list] [name-filter]"); return; }
            other => name_filter = Some(other.to_string()),
        }
    }
    let engines: Vec<Engine> = Engine::ALL.into_iter().filter(|e| engine_filter.map_or(true, |f| f == *e)).collect();
    let matches = |g: &str, c: &str| name_filter.as_ref().map_or(true, |n| g.contains(n.as_str()) || c.contains(n.as_str()));

    if list {
        for g in cases::all() {
            println!("{}", g.name);
            for c in &g.cases { println!("  {}  [{}]", c.name, c.engines.iter().map(|e| e.arch()).collect::<Vec<_>>().join(",")); }
        }
        return;
    }

    // Engine availability banner.
    eprintln!("engines: {}", Engine::ALL.iter()
        .map(|e| format!("{} {}", e.arch(), if e.available() { "✓" } else { "✗(not built)" }))
        .collect::<Vec<_>>().join("   "));

    let ctx = Ctx::discover();
    let (mut pass, mut fail, mut skip) = (0u32, 0u32, 0u32);
    let mut failures: Vec<String> = Vec::new();

    for g in cases::all() {
        let group_cases: Vec<_> = g.cases.iter().filter(|c| matches(g.name, c.name)).collect();
        if group_cases.is_empty() { continue; }
        println!("\n\x1b[1m{}\x1b[0m", g.name);
        for c in group_cases {
            print!("  {:<14}", c.name);
            for &e in &engines {
                match run(&ctx, c, e) {
                    Status::Pass => { pass += 1; print!("  \x1b[32m✓\x1b[0m {}", e.arch()); }
                    Status::Skip(_) => { skip += 1; print!("  \x1b[90m· {}\x1b[0m", e.arch()); }
                    Status::Fail(m) => { fail += 1; print!("  \x1b[31m✗ {}\x1b[0m", e.arch());
                        failures.push(format!("{}/{} [{}]: {}", g.name, c.name, e.arch(), m)); }
                }
            }
            println!();
        }
    }

    println!("\n{}", "─".repeat(48));
    for f in &failures { println!("\x1b[31m✗\x1b[0m {f}"); }
    let color = if fail > 0 { "31" } else { "32" };
    println!("\x1b[1;{color}m{pass} passed\x1b[0m  {fail} failed  \x1b[90m{skip} skipped\x1b[0m");
    std::process::exit(if fail > 0 { 1 } else { 0 });
}

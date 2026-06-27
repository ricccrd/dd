//! Bake the app version into the binary as `DD_VERSION`. Source of truth is the **git tag** (CI
//! passes it via `$DD_VERSION`; locally we read `git describe --tags`), falling back to the crate
//! version. The updater and Settings read `env!("DD_VERSION")` — never the Cargo.toml version
//! directly — so a release tag drives both "what am I" and the `.dmg` name (via the Makefile).

use std::process::Command;

fn main() {
    let version = std::env::var("DD_VERSION")
        .ok()
        .filter(|s| !s.is_empty())
        .or_else(git_tag)
        .unwrap_or_else(|| env!("CARGO_PKG_VERSION").to_string());
    println!("cargo:rustc-env=DD_VERSION={version}");
    println!("cargo:rerun-if-env-changed=DD_VERSION");
}

/// Latest tag, e.g. `v0.2.0` -> `0.2.0`. `None` if not in a git checkout / no tags.
fn git_tag() -> Option<String> {
    let out = Command::new("git").args(["describe", "--tags", "--abbrev=0"]).output().ok()?;
    if !out.status.success() {
        return None;
    }
    let s = String::from_utf8_lossy(&out.stdout).trim().trim_start_matches('v').to_string();
    (!s.is_empty()).then_some(s)
}

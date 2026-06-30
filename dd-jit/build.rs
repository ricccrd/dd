// dd-jit build script: compile + codesign the JIT binaries for every supported guest arch.
//
// The JIT needs the macOS toolchain (arm64 codegen + MAP_JIT + a codesigned `allow-jit` entitlement),
// so on a non-macOS dev host we drive clang/codesign through the `mac` bridge. On a real macOS host we
// invoke them directly. Each guest target is one unity TU (src/runtime/targets/<target>.c) -> one
// executable in OUT_DIR, whose path is exported to the crate via `cargo:rustc-env=DDJIT_<TARGET>`.
// Targets span the guest-OS × guest-ISA matrix: linux_aarch64 (jit), linux_x86_64 (jit86),
// darwin_aarch64 (jitdarwin — native macOS Mach-O containers).
use std::env;
use std::path::PathBuf;
use std::process::Command;

const TARGETS: &[&str] = &["linux_aarch64", "linux_x86_64", "darwin_aarch64"];

fn main() {
    let manifest = PathBuf::from(env::var("CARGO_MANIFEST_DIR").unwrap());
    let runtime = manifest.join("src/runtime");
    let out = PathBuf::from(env::var("OUT_DIR").unwrap());
    let ent = manifest.join("jit.entitlements");

    // Recompile if any C source or the entitlements change.
    println!("cargo:rerun-if-changed={}", ent.display());
    rerun_dir(&runtime);

    let on_mac = env::var("CARGO_CFG_TARGET_OS").as_deref() == Ok("macos");
    let mut built = Vec::new();
    for t in TARGETS {
        let tu = runtime.join("targets").join(format!("{t}.c"));
        let bin = out.join(format!("ddjit-{t}"));
        if !tu.exists() {
            println!("cargo:warning=skipping {t}: {} not found", tu.display());
            continue;
        }
        let script = format!(
            "clang -O2 -o {bin} {tu} && codesign -s - --entitlements {ent} -f {bin}",
            bin = sh(&bin), tu = sh(&tu), ent = sh(&ent),
        );
        let status = if on_mac {
            Command::new("bash").arg("-lc").arg(&script).status()
        } else {
            // dev host (Linux/OrbStack): reach the macOS toolchain through the `mac` bridge.
            Command::new("mac").arg("bash").arg("-lc").arg(&script).status()
        };
        match status {
            Ok(s) if s.success() => {
                println!("cargo:rustc-env=DDJIT_{}={}", t.to_uppercase(), bin.display());
                built.push(*t);
            }
            // On a real macOS host the engine MUST compile -- a missing ddjit-<t> ships a bundle that hangs the
            // moment a guest of that arch runs. Fail the build loudly instead of silently degrading to a warning
            // (the failure mode that shipped a darwin-only release). On a non-mac dev host the toolchain reaches
            // through the `mac` bridge, which may legitimately be absent, so there we stay best-effort.
            Ok(s) if on_mac => panic!("building ddjit-{t} failed ({s}); fix the C engine compile -- refusing to produce an engine-incomplete build"),
            Err(e) if on_mac => panic!("cannot build ddjit-{t} ({e}); macOS toolchain (clang/codesign) must be present"),
            Ok(s) => println!("cargo:warning=building ddjit-{t} failed ({s}); binary unavailable"),
            Err(e) => println!("cargo:warning=cannot build ddjit-{t} ({e}); is the toolchain/`mac` bridge present?"),
        }
    }
    // Always define the env vars (empty if a build failed) so lib.rs `env!` compiles.
    for t in TARGETS {
        if !built.contains(t) { println!("cargo:rustc-env=DDJIT_{}=", t.to_uppercase()); }
    }

    // darwinjail: the DYLD-interposing jail dylib for native macOS containers (`ddcli mac`). Runs the
    // host's arm64 binaries jailed -- no DBT. arm64 only (matches the userland); exported as DDJAIL_*.
    let djc = runtime.join("os/darwin/jail/jail.c");
    let djdylib = out.join("darwinjail.dylib");
    let mut jail_built = false;
    if djc.exists() {
        let script = format!(
            "clang -arch arm64 -O2 -dynamiclib -o {o} {c} && codesign -s - -f {o}",
            o = sh(&djdylib), c = sh(&djc),
        );
        let status = if on_mac {
            Command::new("bash").arg("-lc").arg(&script).status()
        } else {
            Command::new("mac").arg("bash").arg("-lc").arg(&script).status()
        };
        match status {
            Ok(s) if s.success() => {
                println!("cargo:rustc-env=DDJAIL_DARWIN_AARCH64={}", djdylib.display());
                jail_built = true;
            }
            Ok(s) => println!("cargo:warning=building darwinjail failed ({s})"),
            Err(e) => println!("cargo:warning=cannot build darwinjail ({e})"),
        }
    }
    if !jail_built { println!("cargo:rustc-env=DDJAIL_DARWIN_AARCH64="); }
}

fn rerun_dir(dir: &std::path::Path) {
    if let Ok(rd) = std::fs::read_dir(dir) {
        for e in rd.flatten() {
            let p = e.path();
            if p.is_dir() { rerun_dir(&p); }
            else if matches!(p.extension().and_then(|s| s.to_str()), Some("c" | "h")) {
                println!("cargo:rerun-if-changed={}", p.display());
            }
        }
    }
}

fn sh(p: &std::path::Path) -> String { format!("'{}'", p.display()) }

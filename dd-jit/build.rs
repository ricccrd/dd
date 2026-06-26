// dd-jit build script: compile + codesign the JIT binaries for every supported guest arch.
//
// The JIT needs the macOS toolchain (arm64 codegen + MAP_JIT + a codesigned `allow-jit` entitlement),
// so on a non-macOS dev host we drive clang/codesign through the `mac` bridge. On a real macOS host we
// invoke them directly. Each guest arch is one unity TU (src/runtime/ddjit_<arch>.c) -> one executable
// in OUT_DIR, whose path is exported to the crate via `cargo:rustc-env=DDJIT_<ARCH>`.
use std::env;
use std::path::PathBuf;
use std::process::Command;

const GUESTS: &[&str] = &["aarch64", "x86_64"];

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
    for arch in GUESTS {
        let tu = runtime.join(format!("ddjit_{arch}.c"));
        let bin = out.join(format!("ddjit-{arch}"));
        if !tu.exists() {
            println!("cargo:warning=skipping {arch}: {} not found", tu.display());
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
                println!("cargo:rustc-env=DDJIT_{}={}", arch.to_uppercase(), bin.display());
                built.push(*arch);
            }
            Ok(s) => println!("cargo:warning=building ddjit-{arch} failed ({s}); binary unavailable"),
            Err(e) => println!("cargo:warning=cannot build ddjit-{arch} ({e}); is the toolchain/`mac` bridge present?"),
        }
    }
    // Always define the env vars (empty if a build failed) so lib.rs `option_env!` compiles.
    for arch in GUESTS {
        if !built.contains(arch) { println!("cargo:rustc-env=DDJIT_{}=", arch.to_uppercase()); }
    }
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

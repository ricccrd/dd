//! Rust — rustc drives LLVM codegen then links and runs the produced binary: a real native-codegen
//! pipeline (cc1-class compile + ld + exec) inside the container. musl (alpine) + glibc (slim).
//! Known gap: rustc hits the exec-loader gap on dd (GAPS exec-loader-noent) → xfail BOTH linux arches.
//! Must still PASS on the Real oracle. Floating 1.x tags (rust:1-*) for stable availability.

use crate::scenario::{scen, Scenario, Target};

pub fn scenarios() -> Vec<Scenario> {
    vec![
        scen("languages/rust-sum-1-alpine", "rust:1-alpine")
            .exec("cat > /m.rs <<'EOF'\nfn main(){ let s:u64=(1..=1000).sum(); println!(\"{}\",s); }\nEOF\nrustc /m.rs -o /m && /m")
            .has("500500")
            .long()
            .xfail(&Target::LINUX), // GAPS exec-loader-noent
        scen("languages/rust-fib-1-slim", "rust:1-slim")
            .exec("cat > /m.rs <<'EOF'\nfn main(){ let (mut a,mut b):(u64,u64)=(0,1); for _ in 0..50 {let t=a+b;a=b;b=t;} println!(\"{}\",a); }\nEOF\nrustc /m.rs -o /m && /m")
            .has("12586269025")
            .long()
            .xfail(&Target::LINUX),
        scen("languages/rust-version-1-slim", "rust:1-slim")
            .exec("rustc --version | grep -o 'rustc 1.'")
            .has("rustc 1.")
            .xfail(&Target::LINUX),
    ]
}

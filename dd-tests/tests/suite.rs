//! `cargo test -p dd-tests` runs the whole engine × case matrix; any failed case fails the test.
//! For granular grouped output + filtering use the runner: `cargo run -p dd-tests`.
use dd_tests::{cases, run, Ctx, Engine, Status};

#[test]
fn matrix() {
    let ctx = Ctx::discover();
    let (mut ran, mut failures) = (0u32, Vec::<String>::new());
    for g in cases::all() {
        for c in &g.cases {
            for e in Engine::ALL {
                match run(&ctx, c, e) {
                    Status::Pass => ran += 1,
                    Status::Skip(_) => {}
                    Status::Fail(m) => failures.push(format!("{}/{} [{}]: {}", g.name, c.name, e.arch(), m)),
                }
            }
        }
    }
    assert!(failures.is_empty(), "{} case(s) ran; failures:\n{}", ran, failures.join("\n"));
}

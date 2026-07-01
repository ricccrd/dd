//! The real-software scenario registry. Each category is its OWN folder (`src/scenarios/<cat>/`) owned
//! by one builder agent — agents never edit a shared file, so many run in parallel without collision.
//! This file just declares the folders and aggregates them; add a category = add a folder + two lines.
//!
//! Authoring contract (see docs/CHARTER.md, docs/TESTING.md, docs/IMAGE-MANIFEST.md):
//!   * verify every case against `--backend real` (host docker = ground-truth oracle) so the TEST is
//!     proven correct; then `--backend dd` reveals JIT divergences.
//!   * runs on BOTH linux arches by default; pin output (deterministic); known dd gaps → `.xfail()`.

use crate::scenario::ScenGroup;

pub mod distros;
pub mod databases;
pub mod languages;
pub mod web;
pub mod toolchains;
pub mod utilities;
pub mod weird;
pub mod terminal;
// Core container-behaviour regression net (fast, no heavy installs):
pub mod filesystem;   // rootfs + overlay VFS (no volume)
pub mod volumes;      // -v bind mounts (incl. #118 nested `..`)
pub mod networking;   // single-container loopback / DNS / gated outbound
pub mod netcontainer; // between containers on a user-defined network
pub mod process;      // env / workdir / exit / streams / signals / exec

pub fn all() -> Vec<ScenGroup> {
    vec![
        distros::group(),
        databases::group(),
        languages::group(),
        web::group(),
        toolchains::group(),
        utilities::group(),
        weird::group(),
        terminal::group(),
        filesystem::group(),
        volumes::group(),
        networking::group(),
        netcontainer::group(),
        process::group(),
    ]
}

#![allow(unused_imports, dead_code)]
use crate::model::*;
use crate::system::*;
use crate::images::*;
use crate::containers::*;
use crate::build::*;
use crate::archive::*;
use crate::volumes::*;
use crate::networks::*;
use crate::runtime::*;
use crate::registry::{Client, Credentials, ImageRef};
use axum::body::Body;
use axum::extract::{Path, Query, Request, State};
use axum::http::{StatusCode, Uri, HeaderMap};
use axum::response::{IntoResponse, Response};
use axum::Json;
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use std::collections::HashMap;
use std::path::PathBuf;
use std::sync::Arc;
use std::time::{SystemTime, UNIX_EPOCH};
use std::process::Stdio;
use std::os::fd::{AsRawFd, FromRawFd, OwnedFd, RawFd};
use tokio::io::unix::AsyncFd;
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::sync::{broadcast, mpsc, watch, Mutex};
use hyper_util::rt::TokioIo;
use ddjit::{Guest, PortMap, SpawnConfig, Volume};


pub(crate) const API_VERSION: &str = "1.43";


/// Resolve a container ref (full id, **id prefix** like the docker CLI sends, or short name) to its
/// full map key. Docker clients show/round-trip the 12-char short id, so prefix resolution is
/// required for `docker logs/inspect/rm <shortid>` to work.
pub(crate) fn resolve_cid(g: &Inner, id: &str) -> Option<String> {
    if g.containers.contains_key(id) {
        return Some(id.to_string());
    }
    let hits: Vec<String> = g.containers.keys().filter(|k| k.starts_with(id)).cloned().collect();
    if hits.len() == 1 {
        return hits.into_iter().next();
    }
    // Fall back to the short-id "name" we expose in containers_json, then the user-assigned --name.
    let want = id.trim_start_matches('/');
    g.containers.keys().find(|k| k.get(..12).map(|p| p == want).unwrap_or(false)).cloned()
        .or_else(|| g.containers.iter().find(|(_, c)| c.name == want).map(|(k, _)| k.clone()))
}


/// One Docker log frame: `[stream(1B), 0,0,0, len(4B big-endian)] + payload`.
pub(crate) fn log_frame(stream: u8, data: &[u8]) -> Vec<u8> {
    let mut out = Vec::with_capacity(8 + data.len());
    out.push(stream);
    out.extend_from_slice(&[0, 0, 0]);
    out.extend_from_slice(&(data.len() as u32).to_be_bytes());
    out.extend_from_slice(data);
    out
}

pub(crate) fn no_such(id: &str) -> Response {
    (StatusCode::NOT_FOUND, Json(json!({"message": format!("No such container: {id}")}))).into_response()
}


/// A cheap, stable hex id for a built image (not a real digest — just a handle for the CLI).
pub(crate) fn md5_like(s: &str) -> u64 {
    let mut h: u64 = 0xcbf29ce484222325;
    for b in s.bytes() { h ^= b as u64; h = h.wrapping_mul(0x100000001b3); }
    h
}


/// Standard base64 (no line breaks).
pub(crate) fn base64_std(data: &[u8]) -> String {
    const A: &[u8] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    let mut out = String::new();
    for chunk in data.chunks(3) {
        let n = (chunk[0] as u32) << 16 | (*chunk.get(1).unwrap_or(&0) as u32) << 8 | *chunk.get(2).unwrap_or(&0) as u32;
        out.push(A[(n >> 18 & 63) as usize] as char);
        out.push(A[(n >> 12 & 63) as usize] as char);
        out.push(if chunk.len() > 1 { A[(n >> 6 & 63) as usize] as char } else { '=' });
        out.push(if chunk.len() > 2 { A[(n & 63) as usize] as char } else { '=' });
    }
    out
}


// ---- persistence -----------------------------------------------------------

/// `~/.dd` (or `./.dd` if `$HOME` is unset) — the default state/volumes root.
pub(crate) fn dd_home() -> PathBuf {
    std::env::var_os("HOME").map(PathBuf::from).unwrap_or_else(|| PathBuf::from(".")).join(".dd")
}


/// `~/.dd/buildcache` — the `docker build` layer cache root (one dir per cached step under `layers/`).
/// Distinct from `~/.dd/pcache` (the JIT translated-code cache surfaced as `system df` BuilderSize).
pub(crate) fn buildcache_dir() -> PathBuf {
    dd_home().join("buildcache")
}


pub(crate) fn now_secs() -> i64 {
    SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_secs() as i64).unwrap_or(0)
}


/// Format a unix timestamp as an RFC3339 UTC string (Docker's inspect `Created` is a string).
/// Pure integer civil-date math (Howard Hinnant's algorithm) — no chrono dependency.
pub(crate) fn fmt_rfc3339(secs: i64) -> String {
    let days = secs.div_euclid(86400);
    let tod = secs.rem_euclid(86400);
    let (hh, mm, ss) = (tod / 3600, (tod % 3600) / 60, tod % 60);
    let z = days + 719468;
    let era = (if z >= 0 { z } else { z - 146096 }) / 146097;
    let doe = z - era * 146097;
    let yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    let y = yoe + era * 400;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = doy - (153 * mp + 2) / 5 + 1;
    let mo = if mp < 10 { mp + 3 } else { mp - 9 };
    let y = if mo <= 2 { y + 1 } else { y };
    format!("{y:04}-{mo:02}-{d:02}T{hh:02}:{mm:02}:{ss:02}Z")
}


/// Write containers/volumes/networks to `path` atomically (temp file + rename). Best-effort:
/// persistence failures are logged but never abort a request.
pub(crate) fn save_state(inner: &Inner, path: &str) {
    let p = Persisted {
        containers: inner.containers.values().cloned().collect(),
        volumes: inner.volumes.clone(),
        networks: inner.networks.clone(),
    };
    let Ok(bytes) = serde_json::to_vec_pretty(&p) else { return };
    if let Some(parent) = std::path::Path::new(path).parent() {
        let _ = std::fs::create_dir_all(parent);
    }
    let tmp = format!("{path}.tmp");
    if std::fs::write(&tmp, &bytes).is_ok() {
        if let Err(e) = std::fs::rename(&tmp, path) {
            eprintln!("[dd-daemon] state save failed: {e}");
        }
    }
}


/// Load persisted state into `inner`, re-resolving each container's arch/rootfs from the
/// freshly discovered images.
pub(crate) fn load_state(inner: &mut Inner, path: &str) {
    let Ok(bytes) = std::fs::read(path) else { return };
    let Ok(p) = serde_json::from_slice::<Persisted>(&bytes) else {
        eprintln!("[dd-daemon] ignoring unreadable state file {path}");
        return;
    };
    for mut c in p.containers {
        if let Some(img) = inner.images.iter().find(|i| i.name == c.image || format!("{}:latest", i.name) == c.image) {
            c.arch = Some(img.arch);
            c.rootfs = img.rootfs.clone();
        } else {
            c.arch = Some(Guest::LinuxAarch64);
        }
        inner.containers.insert(c.id.clone(), c);
    }
    inner.volumes = p.volumes;
    inner.networks = p.networks;
}


/// Discover <images>/<name>/rootfs dirs, detecting each image's guest arch from a probe ELF.
pub(crate) fn discover_images(images_dir: &str) -> Vec<Image> {
    let mut out = Vec::new();
    let Ok(rd) = std::fs::read_dir(images_dir) else { return out };
    for e in rd.flatten() {
        let rootfs = e.path().join("rootfs");
        if !rootfs.is_dir() { continue; }
        // Prefer dd-image.json so name/cmd/os round-trip exactly (macOS images have no probe-able ELF);
        // else parse the dir name + detect the arch from a probe binary.
        let meta = std::fs::read_to_string(e.path().join("dd-image.json")).ok()
            .and_then(|s| serde_json::from_str::<Value>(&s).ok());
        let (name, cmd, arch) = match &meta {
            Some(m) => {
                let name = m["name"].as_str().unwrap_or("img").to_string();
                let cmd: Vec<String> = m["cmd"].as_array().map(|a| a.iter().filter_map(|x| x.as_str().map(String::from)).collect()).unwrap_or_default();
                // Prefer the arch the sidecar recorded at pull/build time (round-trips exactly, even for
                // images whose binaries can't be sniffed — distroless/scratch). `os:darwin` marks a
                // native-macOS (darwinjail) image. Fall back to probing the rootfs, then native arm64.
                let arch = m["arch"].as_str().and_then(|a| Guest::detect(m["os"].as_str().unwrap_or("linux"), a))
                    .or_else(|| (m["os"].as_str() == Some("darwin")).then_some(Guest::DarwinAarch64))
                    .or_else(|| detect_arch(&rootfs))
                    .unwrap_or(Guest::LinuxAarch64);
                (name, if cmd.is_empty() { vec!["/bin/sh".into()] } else { cmd }, arch)
            }
            None => {
                let raw = e.path().file_name().and_then(|s| s.to_str()).unwrap_or("img").to_string();
                let name = raw.trim_end_matches("-bundle").split("__").next().unwrap_or("img").rsplit('_').next().unwrap_or("img").to_string();
                (name, vec!["/bin/sh".into()], detect_arch(&rootfs).unwrap_or(Guest::LinuxAarch64))
            }
        };
        let arr = |k: &str| meta.as_ref().and_then(|m| m[k].as_array())
            .map(|a| a.iter().filter_map(|x| x.as_str().map(String::from)).collect::<Vec<_>>()).unwrap_or_default();
        let entrypoint = arr("entrypoint");
        let workdir = meta.as_ref().and_then(|m| m["workdir"].as_str()).unwrap_or("").to_string();
        let created = std::fs::metadata(&rootfs).and_then(|m| m.modified()).ok()
            .and_then(|t| t.duration_since(std::time::UNIX_EPOCH).ok()).map(|d| d.as_secs() as i64).unwrap_or(0);
        // The sidecar is the source of truth, but pre-seeded/umoci-built images (and any image cached
        // before the pull path recorded env) carry an empty `env` — their environment lives only in the
        // on-disk OCI config. Recover it from there so a daemon restart doesn't drop TERM/HOME/LANG/PATH,
        // then persist it back into the sidecar so subsequent discovery is self-contained.
        let mut env = arr("env");
        if env.is_empty() {
            let recovered = oci_disk_env(&e.path());
            if !recovered.is_empty() {
                persist_discovered_env(&e.path(), meta.as_ref(), &name, &cmd, &recovered, &entrypoint, &workdir, arch);
                env = recovered;
            }
        }
        out.push(Image { name, rootfs: rootfs.to_string_lossy().into_owned(), arch, cmd, env, entrypoint, workdir, created, ..Default::default() });
    }
    dedup_images(out)
}


/// A coarse "richness" score for a discovered [`Image`], used to pick the best entry when several
/// directories resolve to the same image (see [`dedup_images`] / [`find_image`]). A non-empty
/// environment is the decisive signal: `poc/images` ships some images twice — a single-underscore
/// dd-format dir whose sidecar recorded an empty `env`, AND a umoci bundle dir carrying the full OCI
/// config — and the bundle one (real env) must win. The remaining run metadata break finer ties.
pub(crate) fn image_score(img: &Image) -> i32 {
    let mut s = 0;
    if !img.env.is_empty() { s += 1000; }
    if !img.entrypoint.is_empty() { s += 10; }
    if !img.workdir.is_empty() { s += 5; }
    s += img.labels.len() as i32;
    // A recorded CMD beats the `/bin/sh` default the discovery fallback substitutes.
    if img.cmd.len() != 1 || img.cmd[0] != "/bin/sh" { s += 1; }
    s
}

/// Collapse images that resolve to the same `repository:tag` down to a single best entry so lookup is
/// deterministic regardless of `read_dir` order. Without this, two on-disk dirs for one tag (the
/// dd-format + umoci-bundle duplicates `poc/images` ships) would both enter the store and `docker
/// inspect`/`run` would surface whichever the filesystem happened to list first — sometimes the
/// empty-env one. Ranks by [`image_score`] (richest wins) and breaks exact ties on the name string so
/// the survivor is stable across runs and machines.
fn dedup_images(mut imgs: Vec<Image>) -> Vec<Image> {
    // Best-first ordering (then name, for a deterministic tie-break); keep the first seen per tag.
    imgs.sort_by(|a, b| image_score(b).cmp(&image_score(a)).then_with(|| a.name.cmp(&b.name)));
    let mut seen = std::collections::HashSet::new();
    imgs.retain(|i| seen.insert(repo_tag(&i.name)));
    imgs
}

/// Pick the single best image matching a docker reference (`docker inspect ubuntu`, `docker run
/// alpine`), deterministically. dd's lookup is lenient — a bare name matches any stored image with that
/// repository regardless of tag (see [`ref_name`]) — so several images can match one query (e.g. an
/// `ubuntu` and an `ubuntu:24.04`). Returning `Iterator::find`'s first hit made the result depend on
/// insertion order; instead rank the candidates and return the best:
///   1. an exact `repository:tag` match for the requested reference wins, then `<name>:latest`,
///   2. then the richest metadata (a real environment beats an empty one — see [`image_score`]),
///   3. then the name string (reversed so the lexicographically smallest wins) to settle any remainder.
pub(crate) fn find_image<'a>(images: &'a [Image], reference: &str) -> Option<&'a Image> {
    let want = ref_name(reference);
    let want_rt = repo_tag(reference);
    let want_latest = format!("{want}:latest");
    images.iter()
        .filter(|i| ref_name(&i.name) == want)
        .max_by_key(|i| {
            (repo_tag(&i.name) == want_rt, repo_tag(&i.name) == want_latest,
             image_score(i), std::cmp::Reverse(i.name.clone()))
        })
}


/// Best-effort recovery of an image's environment from an on-disk OCI config, used by
/// [`discover_images`] when the `dd-image.json` sidecar recorded no `env` (pre-seeded / umoci-built
/// images, or images cached before the pull path persisted env). Two layouts are understood, in order:
///   1. umoci's runtime `config.json` at the image dir root -> `process.env`.
///   2. an OCI image layout in the dir (`index.json` + `blobs/sha256/`) -> manifest -> image config
///      blob -> `config.Env`.
/// Returns an empty vec if neither is present/parseable — never panics, never fails discovery.
fn oci_disk_env(dir: &std::path::Path) -> Vec<String> {
    let strs = |v: &Value| v.as_array()
        .map(|a| a.iter().filter_map(|x| x.as_str().map(String::from)).collect::<Vec<_>>())
        .unwrap_or_default();
    // 1. umoci runtime config: process.env.
    if let Some(cfg) = std::fs::read_to_string(dir.join("config.json")).ok()
        .and_then(|s| serde_json::from_str::<Value>(&s).ok())
    {
        let env = strs(&cfg["process"]["env"]);
        if !env.is_empty() { return env; }
    }
    // 2. OCI image layout: index.json -> first manifest blob -> image config blob -> config.Env.
    let read_blob = |digest: &str| -> Option<Value> {
        let hex = digest.strip_prefix("sha256:")?;
        std::fs::read_to_string(dir.join("blobs/sha256").join(hex)).ok()
            .and_then(|s| serde_json::from_str::<Value>(&s).ok())
    };
    let index = std::fs::read_to_string(dir.join("index.json")).ok()
        .and_then(|s| serde_json::from_str::<Value>(&s).ok());
    if let Some(mdigest) = index.as_ref()
        .and_then(|i| i["manifests"].as_array()).and_then(|a| a.first())
        .and_then(|m| m["digest"].as_str())
    {
        if let Some(cfg) = read_blob(mdigest)
            .and_then(|m| m["config"]["digest"].as_str().map(String::from))
            .and_then(|d| read_blob(&d))
        {
            return strs(&cfg["config"]["Env"]);
        }
    }
    Vec::new()
}

/// Persist an env recovered by [`oci_disk_env`] back into the image's `dd-image.json` sidecar so the
/// next discovery round-trips it directly (and never has to re-parse the OCI config). Merges into the
/// existing sidecar when present so other recorded fields are preserved; otherwise writes a fresh one
/// from the values [`discover_images`] already resolved. Best-effort: a write failure (e.g. a
/// read-only image store) is ignored — the in-memory env is still surfaced for this run.
#[allow(clippy::too_many_arguments)]
fn persist_discovered_env(dir: &std::path::Path, meta: Option<&Value>, name: &str, cmd: &[String],
                          env: &[String], entrypoint: &[String], workdir: &str, arch: Guest) {
    let mut m = meta.cloned().unwrap_or_else(|| json!({
        "name": name, "cmd": cmd, "entrypoint": entrypoint, "workdir": workdir,
        "arch": arch.arch(), "os": arch.os(),
    }));
    m["env"] = json!(env);
    let _ = std::fs::write(dir.join("dd-image.json"), m.to_string());
}


/// Classify a binary by its leading magic bytes: ELF -> linux (e_machine = aarch64/x86_64),
/// Mach-O 64 -> darwin (cputype = arm64). Returns `None` for anything else (scripts, data, an
/// unrecognized machine).
fn sniff_magic(b: &[u8]) -> Option<Guest> {
    if b.len() > 19 && &b[0..4] == b"\x7fELF" {
        return match u16::from_le_bytes([b[18], b[19]]) {  // ELF e_machine
            0xB7 => Some(Guest::LinuxAarch64),
            0x3E => Some(Guest::LinuxX86_64),
            _ => None,
        };
    }
    if b.len() > 7 && b[0..4] == [0xCF, 0xFA, 0xED, 0xFE] {   // MH_MAGIC_64 (little-endian)
        return match u32::from_le_bytes([b[4], b[5], b[6], b[7]]) {  // cputype
            0x0100000C => Some(Guest::DarwinAarch64),   // CPU_TYPE_ARM64
            _ => None,
        };
    }
    None
}

/// Read just the header of `p` (following symlinks) and classify its magic. Cheap: only the first 20
/// bytes are read, never the whole binary.
fn sniff_path(p: &std::path::Path) -> Option<Guest> {
    use std::io::Read;
    let mut f = std::fs::File::open(p).ok()?;
    let mut buf = [0u8; 20];
    let n = f.read(&mut buf).ok()?;
    sniff_magic(&buf[..n])
}

/// Fallback arch probe: a bounded breadth-first scan of the rootfs for the first binary whose magic
/// identifies a guest target. Catches images that ship a single executable at a non-standard path
/// (hello-world's `/hello`, nats's `/nats-server`) which the fixed probe list in [`detect_arch`] misses.
/// Shallow entries are examined first (top-level binaries win immediately) and the total entry budget is
/// capped so a large rootfs can never make discovery pathological. Symlinked directories are not
/// descended (avoids cycles); symlinked *files* are still classified (their target is read).
fn scan_for_binary(rootfs: &std::path::Path) -> Option<Guest> {
    use std::collections::VecDeque;
    let mut queue = VecDeque::from([rootfs.to_path_buf()]);
    let mut budget = 4096; // cap on entries examined across the whole walk
    while let Some(dir) = queue.pop_front() {
        let Ok(rd) = std::fs::read_dir(&dir) else { continue };
        for e in rd.flatten() {
            if budget == 0 { return None; }
            budget -= 1;
            match e.file_type() {
                Ok(ft) if ft.is_dir() => queue.push_back(e.path()),
                Ok(ft) if ft.is_file() || ft.is_symlink() => {
                    if let Some(g) = sniff_path(&e.path()) { return Some(g); }
                }
                _ => {}
            }
        }
    }
    None
}

/// Probe the rootfs and pick the guest target from a binary's magic. Tries a handful of well-known
/// executable locations first (fast path), then falls back to a bounded scan of the whole rootfs so an
/// image with its binary at a non-standard path is still detected.
pub(crate) fn detect_arch(rootfs: &std::path::Path) -> Option<Guest> {
    // Includes darwin-userland paths (`profile/bin/*`, `opt/homebrew/bin/*`) so a *pulled* macOS image
    // — whose `dd-image.json` sidecar didn't survive the registry round-trip — is still detected as
    // darwin from its packed Mach-O binaries. `sniff_path` follows the profile symlinks to the real
    // Mach-O in the packed `/nix` (or Homebrew) closure.
    for probe in ["bin/busybox", "bin/sh", "bin/true", "usr/bin/coreutils", "usr/lib/dyld",
                  "profile/bin/bash", "profile/bin/sh", "opt/homebrew/bin/brew"] {
        if let Some(g) = sniff_path(&rootfs.join(probe)) { return Some(g); }
    }
    scan_for_binary(rootfs)
}


pub(crate) fn fake_id(seed: &str) -> String {
    let mut h: u64 = 1469598103934665603;
    for b in seed.bytes() { h ^= b as u64; h = h.wrapping_mul(1099511628211); }
    format!("{h:016x}{h:016x}{h:016x}{h:08x}")
}


/// On-disk size of an image's rootfs, cached per rootfs path (computed once; rootfs rarely changes).
/// The host-fs `macos` image is skipped (walking `/` would be catastrophic).
pub(crate) fn image_size(rootfs: &str, name: &str) -> i64 {
    if name == "macos" {
        return 0;
    }
    use std::sync::{Mutex, OnceLock};
    static CACHE: OnceLock<Mutex<HashMap<String, i64>>> = OnceLock::new();
    let cache = CACHE.get_or_init(|| Mutex::new(HashMap::new()));
    if let Some(s) = cache.lock().unwrap().get(rootfs) {
        return *s;
    }
    let s = dir_size(std::path::Path::new(rootfs));
    cache.lock().unwrap().insert(rootfs.to_string(), s);
    s
}


/// Recursively sum the size of regular files under `p` (symlinks are not followed).
pub(crate) fn dir_size(p: &std::path::Path) -> i64 {
    let mut total = 0i64;
    let Ok(rd) = std::fs::read_dir(p) else { return 0 };
    for e in rd.flatten() {
        let Ok(md) = e.path().symlink_metadata() else { continue };
        let ft = md.file_type();
        if ft.is_symlink() {
            continue;
        } else if ft.is_dir() {
            total += dir_size(&e.path());
        } else {
            total += md.len() as i64;
        }
    }
    total
}


/// The bare repository name of a docker image reference, ignoring registry, namespace and tag/digest:
/// `docker.io/library/ubuntu:latest` -> `ubuntu`, `library/ubuntu` -> `ubuntu`, `ubuntu:22.04` -> `ubuntu`.
/// Lets `docker run ubuntu` match an image discovered/tagged as `ubuntu` regardless of how docker
/// canonicalizes the reference.
pub(crate) fn ref_name(s: &str) -> &str {
    let last = s.rsplit('/').next().unwrap_or(s);
    last.split('@').next().unwrap_or(last).split(':').next().unwrap_or(last)
}


/// A unique container id. Seeded from a never-reset process counter + nanosecond clock, so it stays
/// unique even as containers are created and deleted (a count-based seed would collide after a rm).
pub(crate) fn new_id(image: &str) -> String {
    use std::sync::atomic::{AtomicU64, Ordering};
    static SEQ: AtomicU64 = AtomicU64::new(0);
    let seq = SEQ.fetch_add(1, Ordering::Relaxed);
    let nanos = SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_nanos() as u64).unwrap_or(0);
    fake_id(&format!("{image}-{seq}-{nanos}"))
}

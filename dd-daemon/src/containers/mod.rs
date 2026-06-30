#![allow(unused_imports, dead_code)]
//! Container lifecycle HTTP handlers, split into cohesive submodules:
//!   - `lifecycle` — create/start/stop/kill/restart/pause/unpause/rename/wait/delete
//!   - `exec`      — attach + the `/exec` create/start/inspect flow + PTY resize
//!   - `inspect`   — top/stats/logs/inspect/list (`ps`) + prune/changes/update/export
//!
//! This module keeps the shared request structs/helpers (parse_bind, parse_signal,
//! do_stop, q_truthy, ports_json/ports_map_json) and re-exports every handler with
//! `pub(crate) use`, so the public path `crate::containers::<handler>` (used by the
//! router in main.rs and every `use crate::containers::*` site) is unchanged.
use crate::model::*;
use crate::util::*;
use crate::system::*;
use crate::images::*;
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

mod lifecycle;
mod exec;
mod inspect;
pub(crate) use lifecycle::*;
pub(crate) use exec::*;
pub(crate) use inspect::*;

/// Parse a `-v`/Binds spec `src:dst[:opts]` into `(host_source, container_dest, read_only)`. Docker
/// appends comma-separated options after the destination (e.g. `/h:/c:ro`, `vol:/c:rw,z`); `ro` marks
/// the mount read-only. Returns None for a malformed spec (no destination). Note: the prior code split
/// only on the FIRST colon, so `src:dst:ro` mounted at the literal path "dst:ro" — this fixes that and
/// surfaces the RW flag for inspect.
pub(crate) fn parse_bind(b: &str) -> Option<(&str, &str, bool)> {
    let mut it = b.splitn(3, ':');
    let src = it.next()?;
    let dst = it.next()?;
    let ro = it.next().map(|o| o.split(',').any(|p| p == "ro")).unwrap_or(false);
    if dst.is_empty() { return None; }
    Some((src, dst, ro))
}

/// Map a docker signal token ("SIGTERM"/"TERM"/"15"/"9"/"SIGKILL"/...) to its libc number.
/// Numeric tokens are taken verbatim; names are matched case-insensitively with or without the
/// "SIG" prefix. Anything unrecognised falls back to `default`.
fn parse_signal(s: &str, default: i32) -> i32 {
    let t = s.trim();
    if t.is_empty() { return default; }
    if let Ok(n) = t.parse::<i32>() { return n; }
    match t.to_ascii_uppercase().trim_start_matches("SIG") {
        "TERM" => libc::SIGTERM,
        "KILL" => libc::SIGKILL,
        "INT"  => libc::SIGINT,
        "QUIT" => libc::SIGQUIT,
        "HUP"  => libc::SIGHUP,
        "USR1" => libc::SIGUSR1,
        "USR2" => libc::SIGUSR2,
        "STOP" => libc::SIGSTOP,
        "CONT" => libc::SIGCONT,
        _ => default,
    }
}

/// Signal a container's whole process group. The JIT leader is its own group leader (setpgid at spawn
/// in runtime.rs), so the host processes the guest forks inherit that pgid; `kill(-pgid, sig)` (killpg,
/// pgid == leader pid) reaches the leader AND every forked child, so a multi-process container dies
/// completely instead of leaving orphans. Only if the group signal fails (e.g. the leader is mid-
/// teardown) do we fall back to the leader pid alone. Mirrors lifecycle.rs's `kill_group`.
fn kill_group(pid: i32, sig: i32) {
    unsafe { if libc::kill(-pid, sig) != 0 { libc::kill(pid, sig); } }
}

/// stop: deliver a REAL signal to the live JIT process (same mechanism as pause's
/// `libc::kill(pid, SIGSTOP)`), wait up to `t` seconds for the guest to exit on its own, then
/// SIGKILL if it's still alive. Containers with no live process keep the old mark-exited behavior.
/// The wait polls the reaper-maintained container status without holding the inner lock across the
/// `tokio::time::sleep`, and is bounded by `t` so the handler never hangs indefinitely.
async fn do_stop(a: &App, id: &str, sig: i32, t: i64) -> Response {
    // resolve + grab the live pid, then release the lock before any waiting.
    let (full, pid) = {
        let g = a.inner.lock().await;
        let Some(full) = resolve_cid(&g, id) else { return no_such(id) };
        // Mark a deliberate stop so the RestartPolicy supervisor won't auto-restart this container.
        let pid = g.live.get(&full).map(|l| {
            l.stop_requested.store(true, std::sync::atomic::Ordering::SeqCst);
            *l.pid.lock().unwrap()
        }).flatten();
        (full, pid)
    };
    if let Some(pid) = pid {
        kill_group(pid as i32, sig);                             // whole process group, not just the leader
        // give the guest up to `t` seconds to exit on its own; the spawn reaper (runtime.rs) flips
        // status to "exited" when the process dies, so poll that rather than racing on pid reuse.
        let mut waited = 0i64;
        loop {
            let exited = {
                let g = a.inner.lock().await;
                g.containers.get(&full).map(|c| c.status == "exited").unwrap_or(true)
            };
            if exited { break; }
            if waited >= t * 1000 { kill_group(pid as i32, libc::SIGKILL); break; } // group SIGKILL, not just the leader
            tokio::time::sleep(std::time::Duration::from_millis(100)).await;
            waited += 100;
        }
    }
    // mark exited (as before); the reaper sets the real exit_code when the signalled process dies.
    let mut g = a.inner.lock().await;
    if let Some(c) = g.containers.get_mut(&full) { c.status = "exited".into(); c.finished_at = now_secs(); }
    let (cname, cimage) = g.containers.get(&full).map(|c| (c.name.clone(), c.image.clone())).unwrap_or_default();
    crate::events::emit_event(&a.events, "container", "stop", &full, json!({"name": cname, "image": cimage}));
    save_state(&g, &a.state_path);
    StatusCode::NO_CONTENT.into_response()
}

fn q_truthy(s: &Option<String>) -> bool {
    matches!(s.as_deref(), Some("1") | Some("true") | Some("True"))
}

/// Build the `Ports` array Docker clients expect from our "host:container,..." publish string.
pub(crate) fn ports_json(publish: &str) -> Vec<Value> {
    publish.split(',').filter(|s| !s.is_empty()).filter_map(|p| p.split_once(':')).filter_map(|(h, c)| {
        Some(json!({"PublicPort": h.parse::<u16>().ok()?, "PrivatePort": c.parse::<u16>().ok()?, "Type": "tcp", "IP": "0.0.0.0"}))
    }).collect()
}

/// `NetworkSettings.Ports` map (`{"80/tcp": [{"HostIp","HostPort"}]}`) — the shape `docker port` reads
/// (it panics if `.NetworkSettings` is absent). Distinct from the top-level `Ports` array above.
pub(crate) fn ports_map_json(publish: &str) -> Value {
    let mut m = serde_json::Map::new();
    for p in publish.split(',').filter(|s| !s.is_empty()) {
        if let Some((h, c)) = p.split_once(':') {
            m.insert(format!("{c}/tcp"), json!([{"HostIp": "0.0.0.0", "HostPort": h}]));
        }
    }
    Value::Object(m)
}

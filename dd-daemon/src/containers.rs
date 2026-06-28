#![allow(unused_imports, dead_code)]
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


#[derive(Deserialize)]
pub(crate) struct CreateBody {
    #[serde(rename = "Image")] image: Option<String>,
    #[serde(rename = "Cmd")] cmd: Option<Vec<String>>,
    #[serde(rename = "Env")] env: Option<Vec<String>>,
    #[serde(rename = "Entrypoint")] entrypoint: Option<Vec<String>>,
    #[serde(rename = "Hostname")] hostname: Option<String>,
    #[serde(rename = "Tty")] tty: Option<bool>,
    #[serde(rename = "WorkingDir")] working_dir: Option<String>,
    #[serde(rename = "Labels")] labels: Option<HashMap<String, String>>,
    #[serde(rename = "HostConfig")] host_config: Option<HostConfig>,
}

#[derive(Deserialize)]
pub(crate) struct HostConfig {
    #[serde(rename = "Binds")] binds: Option<Vec<String>>,
    #[serde(rename = "Memory")] memory: Option<i64>,
    #[serde(rename = "PidsLimit")] pids_limit: Option<i64>,
    #[serde(rename = "PortBindings")] port_bindings: Option<HashMap<String, Vec<PortBinding>>>,
    #[serde(rename = "NetworkMode")] network_mode: Option<String>,
}

#[derive(Deserialize, Clone)]
pub(crate) struct PortBinding { #[serde(rename = "HostPort")] host_port: Option<String> }

pub(crate) fn publish_str(pb: &HashMap<String, Vec<PortBinding>>) -> String {
    let mut v = Vec::new();
    for (k, binds) in pb {
        let cport = k.split('/').next().unwrap_or("");
        for b in binds { if let Some(hp) = &b.host_port { if !hp.is_empty() && !cport.is_empty() { v.push(format!("{hp}:{cport}")); } } }
    }
    v.join(",")
}


#[derive(Deserialize)]
pub(crate) struct CreateQ { name: Option<String>, platform: Option<String> }

pub(crate) async fn containers_create(State(a): State<App>, Query(cq): Query<CreateQ>, Json(body): Json<CreateBody>) -> Response {
    let image = body.image.unwrap_or_default();
    let mut g = a.inner.lock().await;
    // Match the image by name and, when --platform is given, by arch. A platform mismatch returns 404 so
    // the docker CLI pulls the right arch (its default --pull=missing won't re-pull otherwise) and retries.
    let want_arch = platform_arch(cq.platform.as_deref());
    let img = match g.images.iter()
        .filter(|i| ref_name(&i.name) == ref_name(&image))
        .find(|i| want_arch.map_or(true, |a| docker_arch(i.arch) == a))
        .cloned()
    {
        Some(i) => i,
        None => return (StatusCode::NOT_FOUND, Json(json!({"message": format!("No such image: {image}")}))).into_response(),
    };
    // Final argv = entrypoint ++ cmd (docker semantics). The entrypoint is the user's --entrypoint or the
    // IMAGE's ENTRYPOINT; a user --entrypoint resets CMD, but the image's own ENTRYPOINT still keeps the
    // image CMD. An empty Cmd falls back to the image default.
    let user_ep = body.entrypoint.is_some();
    let mut argv = body.entrypoint.unwrap_or_else(|| img.entrypoint.clone());
    let cmd = body.cmd.filter(|c| !c.is_empty()).unwrap_or_else(|| if user_ep { vec![] } else { img.cmd.clone() });
    argv.extend(cmd);
    if argv.is_empty() { argv = img.cmd.clone(); }
    let cmd = argv;
    // env = image ENV then `docker run -e` (later wins); working dir = -w or the image WORKDIR.
    let mut env = img.env.clone();
    env.extend(body.env.unwrap_or_default());
    let working_dir = body.working_dir.filter(|w| !w.is_empty()).unwrap_or_else(|| img.workdir.clone());
    let tty = body.tty.unwrap_or(false);
    let id = new_id(&image);
    let hc = body.host_config;
    let c = Container {
        id: id.clone(), image, rootfs: img.rootfs, cmd, arch: Some(img.arch),
        binds: hc.as_ref().and_then(|h| h.binds.clone()).unwrap_or_default(),
        hostname: body.hostname.unwrap_or_default(),
        memory: hc.as_ref().and_then(|h| h.memory).unwrap_or(0),
        pids_limit: hc.as_ref().and_then(|h| h.pids_limit).unwrap_or(0),
        publish: hc.as_ref().and_then(|h| h.port_bindings.as_ref()).map(publish_str).unwrap_or_default(),
        created: now_secs(), tty,
        name: cq.name.unwrap_or_default().trim_start_matches('/').to_string(),
        working_dir, env,
        labels: body.labels.unwrap_or_default(),
        network_mode: hc.as_ref().and_then(|h| h.network_mode.clone()).unwrap_or_default(),
        status: "created".into(), ..Default::default()
    };
    // Join the network now (fixes the bug where `docker run --network X` never added the container to
    // the network's membership/IPAM): pick the target network from --network, defaulting to `bridge`.
    let cname = endpoint_name(&c);
    let net_name = match c.network_mode.as_str() {
        "" | "default" | "bridge" => "bridge",
        "host" | "none" => "",          // no L3 identity
        other => other,                 // a user-defined network by name
    };
    if !net_name.is_empty() { join_network(&mut g.networks, net_name, &id, &cname); }
    crate::events::emit_event(&a.events, "container", "create", &id, json!({"name": c.name, "image": c.image}));
    g.containers.insert(id.clone(), c);
    save_state(&g, &a.state_path);
    (StatusCode::CREATED, Json(json!({"Id": id, "Warnings": []}))).into_response()
}


pub(crate) async fn containers_start(State(a): State<App>, Path(id): Path<String>) -> Response {
    let (c, vols, live) = {
        let mut g = a.inner.lock().await;
        let full = match resolve_cid(&g, &id) { Some(f) => f, None => return no_such(&id) };
        let c = match g.containers.get(&full).cloned() { Some(c) => c, None => return no_such(&id) };
        let live = g.live.entry(full.clone()).or_insert_with(|| Live::new(c.tty)).clone();
        if let Some(cc) = g.containers.get_mut(&full) { cc.status = "running".into(); cc.started_at = now_secs(); }
        (c, g.volumes.clone(), live)
    };
    if std::env::var("DD_DEBUG").is_ok() { eprintln!("[start] {} cmd={:?}", &c.id[..12], c.cmd); }
    spawn_live(&a, &c, &vols, live).await;
    crate::events::emit_event(&a.events, "container", "start", &c.id, json!({"name": c.name, "image": c.image}));
    StatusCode::NO_CONTENT.into_response()
}

#[derive(Deserialize)]
pub(crate) struct StopQ { t: Option<i64>, signal: Option<String> }

#[derive(Deserialize)]
pub(crate) struct KillQ { signal: Option<String> }

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
        let pid = g.live.get(&full).and_then(|l| *l.pid.lock().unwrap());
        (full, pid)
    };
    if let Some(pid) = pid {
        unsafe { libc::kill(pid as i32, sig); }                  // mirror of freeze()'s libc::kill
        // give the guest up to `t` seconds to exit on its own; the spawn reaper (runtime.rs) flips
        // status to "exited" when the process dies, so poll that rather than racing on pid reuse.
        let mut waited = 0i64;
        loop {
            let exited = {
                let g = a.inner.lock().await;
                g.containers.get(&full).map(|c| c.status == "exited").unwrap_or(true)
            };
            if exited { break; }
            if waited >= t * 1000 { unsafe { libc::kill(pid as i32, libc::SIGKILL); } break; }
            tokio::time::sleep(std::time::Duration::from_millis(100)).await;
            waited += 100;
        }
    }
    // mark exited (as before); the reaper sets the real exit_code when the signalled process dies.
    let mut g = a.inner.lock().await;
    if let Some(c) = g.containers.get_mut(&full) { c.status = "exited".into(); c.finished_at = now_secs(); }
    crate::events::emit_event(&a.events, "container", "stop", &full, json!({}));
    save_state(&g, &a.state_path);
    StatusCode::NO_CONTENT.into_response()
}

/// POST /containers/:id/stop?t=N&signal=SIG -- default signal SIGTERM, default t=10s.
pub(crate) async fn containers_stop(State(a): State<App>, Path(id): Path<String>, Query(q): Query<StopQ>) -> Response {
    let sig = q.signal.as_deref().map(|s| parse_signal(s, libc::SIGTERM)).unwrap_or(libc::SIGTERM);
    let t = q.t.unwrap_or(10).max(0);
    do_stop(&a, &id, sig, t).await
}

/// POST /containers/:id/kill?signal=SIG -- default signal SIGKILL, delivered immediately.
pub(crate) async fn containers_kill(State(a): State<App>, Path(id): Path<String>, Query(q): Query<KillQ>) -> Response {
    let mut g = a.inner.lock().await;
    let Some(full) = resolve_cid(&g, &id) else { return no_such(&id) };
    let sig = q.signal.as_deref().map(|s| parse_signal(s, libc::SIGKILL)).unwrap_or(libc::SIGKILL);
    if let Some(pid) = g.live.get(&full).and_then(|l| *l.pid.lock().unwrap()) {
        unsafe { libc::kill(pid as i32, sig); }                  // mirror of freeze()'s libc::kill
    }
    if let Some(c) = g.containers.get_mut(&full) { c.status = "exited".into(); c.finished_at = now_secs(); }
    crate::events::emit_event(&a.events, "container", "kill", &full, json!({}));
    save_state(&g, &a.state_path);
    StatusCode::NO_CONTENT.into_response()
}

/// restart: stop the live process (real signal, via the new stop path) then re-run it in place.
pub(crate) async fn containers_restart(State(a): State<App>, Path(id): Path<String>, Query(q): Query<StopQ>) -> Response {
    let sig = q.signal.as_deref().map(|s| parse_signal(s, libc::SIGTERM)).unwrap_or(libc::SIGTERM);
    let t = q.t.unwrap_or(10).max(0);
    let _ = do_stop(&a, &id, sig, t).await;
    containers_start(State(a), Path(id)).await
}


/// POST /containers/:id/attach -- hijack the connection and stream the guest's IO. `docker run` (no -d)
/// and `docker run -it` use this: stdout/stderr come back framed (raw in TTY mode), and the client's
/// stdin (for -i) is fed to the guest. The hijacked stream closes when the guest exits.
/// Drive a hijacked docker stream against a Live: fan guest stdout/stderr to the client (docker
/// multiplexed frames, or raw bytes in TTY mode) and feed the client's stdin into the guest. Shared by
/// container attach and exec. `rx` is subscribed synchronously so no output is missed if the guest
/// starts producing before the upgrade completes.
pub(crate) fn spawn_hijack_io(on_upgrade: hyper::upgrade::OnUpgrade, live: Arc<Live>, tty: bool) {
    let mut rx = live.out.subscribe();
    let mut exit_rx = live.exit_rx.clone();
    let live_in = live.clone();
    tokio::spawn(async move {
        let Ok(upgraded) = on_upgrade.await else { return };
        let (mut rd, mut wr) = tokio::io::split(TokioIo::new(upgraded));
        let writer = tokio::spawn(async move {
            loop {
                tokio::select! {
                    biased;
                    m = rx.recv() => match m {
                        Ok((kind, chunk)) => {
                            let f = if tty { chunk } else { log_frame(kind, &chunk) };
                            if wr.write_all(&f).await.is_err() { return; }
                        }
                        Err(broadcast::error::RecvError::Lagged(_)) => continue,
                        Err(broadcast::error::RecvError::Closed) => break,
                    },
                    _ = exit_rx.changed() => {
                        while let Ok((kind, chunk)) = rx.try_recv() {
                            let f = if tty { chunk } else { log_frame(kind, &chunk) };
                            let _ = wr.write_all(&f).await;
                        }
                        break;
                    }
                }
            }
            let _ = wr.flush().await;
            let _ = wr.shutdown().await;
        });
        let mut buf = [0u8; 8192];
        loop {
            match rd.read(&mut buf).await {
                Ok(0) | Err(_) => break,
                Ok(n) => { if live_in.stdin_tx.send(buf[..n].to_vec()).await.is_err() { break; } }
            }
        }
        let _ = live_in.stdin_tx.send(Vec::new()).await; // EOF -> close guest stdin
        let _ = writer.await;
    });
}


pub(crate) const HIJACK_HEADERS: [(&str, &str); 3] =
    [("Content-Type", "application/vnd.docker.raw-stream"), ("Connection", "Upgrade"), ("Upgrade", "tcp")];

pub(crate) fn hijack_response() -> Response {
    let mut b = Response::builder().status(StatusCode::SWITCHING_PROTOCOLS);
    for (k, v) in HIJACK_HEADERS { b = b.header(k, v); }
    b.body(Body::empty()).unwrap()
}


pub(crate) async fn containers_attach(State(a): State<App>, Path(id): Path<String>, req: Request) -> Response {
    let (full, tty) = {
        let g = a.inner.lock().await;
        let Some(full) = resolve_cid(&g, &id) else { return no_such(&id) };
        let tty = g.containers.get(&full).map(|c| c.tty).unwrap_or(false);
        (full, tty)
    };
    let live = { let mut g = a.inner.lock().await; g.live.entry(full).or_insert_with(|| Live::new(tty)).clone() };
    spawn_hijack_io(hyper::upgrade::on(req), live, tty);
    hijack_response()
}


#[derive(Deserialize)]
pub(crate) struct ExecCreateBody {
    #[serde(rename = "Cmd")] cmd: Option<Vec<String>>,
    #[serde(rename = "Tty")] tty: Option<bool>,
    #[serde(rename = "Env")] env: Option<Vec<String>>,
    #[serde(rename = "WorkingDir")] working_dir: Option<String>,
    #[serde(rename = "User")] user: Option<String>,
}

/// POST /containers/:id/exec -- create an exec (record the command). Run it with /exec/:id/start.
pub(crate) async fn exec_create(State(a): State<App>, Path(id): Path<String>, Json(body): Json<ExecCreateBody>) -> Response {
    let mut g = a.inner.lock().await;
    let Some(full) = resolve_cid(&g, &id) else { return no_such(&id) };
    let cmd = body.cmd.unwrap_or_default();
    if cmd.is_empty() {
        return (StatusCode::BAD_REQUEST, Json(json!({"message": "No exec command specified"}))).into_response();
    }
    let exec_id = new_id(&format!("exec-{full}"));
    g.execs.insert(exec_id.clone(), Exec { container_id: full, cmd, tty: body.tty.unwrap_or(false), started: false,
        env: body.env.unwrap_or_default(), working_dir: body.working_dir.unwrap_or_default(),
        user: body.user.unwrap_or_default() });
    (StatusCode::CREATED, Json(json!({"Id": exec_id}))).into_response()
}

/// POST /exec/:id/start body: `{"Detach": bool, "Tty": bool}`. Detach => run the exec in the
/// background and return 200 (no connection hijack).
#[derive(Deserialize, Default)]
pub(crate) struct ExecStartBody {
    #[serde(rename = "Detach", default)] detach: bool,
    #[serde(rename = "Tty", default)] tty: bool,
}

/// POST /exec/:id/start -- run the exec command as a fresh JIT in the container's rootfs. With
/// `Detach=false` (the default) stream its IO over the hijacked connection (same path as attach);
/// with `Detach=true` spawn it in the background and return 200 immediately (no upgrade, no wait).
pub(crate) async fn exec_start(State(a): State<App>, Path(id): Path<String>, req: Request) -> Response {
    // Read the (small) JSON start body for Detach, keeping the request parts so the OnUpgrade
    // extension survives for the hijack path. `to_bytes` consumes the body; we rebuild an empty one.
    let (parts, body) = req.into_parts();
    let detach = axum::body::to_bytes(body, 64 * 1024).await.ok()
        .and_then(|b| serde_json::from_slice::<ExecStartBody>(&b).ok())
        .map(|b| b.detach).unwrap_or(false);
    let (temp, vols, live, tty) = {
        let mut g = a.inner.lock().await;
        let Some(exec) = g.execs.get(&id).cloned() else {
            return (StatusCode::NOT_FOUND, Json(json!({"message": format!("no such exec: {id}")}))).into_response();
        };
        let Some(c) = g.containers.get(&exec.container_id).cloned() else { return no_such(&exec.container_id) };
        let mut temp = c; // share the container's rootfs/volumes/arch; distinct id -> own process+netns
        temp.id = id.clone();
        temp.cmd = exec.cmd.clone();
        temp.tty = exec.tty;
        // `docker exec -e/-w`: the exec inherits the container's env and adds `-e` overrides (later wins),
        // and `-w` overrides the working dir. spawn_cfg reads temp.env / temp.working_dir, so set them here.
        // (`-u`/User is captured on the Exec but not applied: SpawnConfig has no user/uid field yet.)
        temp.env.extend(exec.env.iter().cloned());
        if !exec.working_dir.is_empty() { temp.working_dir = exec.working_dir.clone(); }
        let live = Live::new(exec.tty);
        g.live.insert(id.clone(), live.clone());
        if let Some(e) = g.execs.get_mut(&id) { e.started = true; }
        (temp, g.volumes.clone(), live, exec.tty)
    };
    if detach {
        // Detached exec: spawn the process in the background (spawn_live already runs+reaps it in a
        // task) and return 200 immediately. No hijack, so the client doesn't block.
        spawn_live(&a, &temp, &vols, live).await;
        return StatusCode::OK.into_response();
    }
    let req = Request::from_parts(parts, Body::empty()); // carries OnUpgrade in extensions
    spawn_hijack_io(hyper::upgrade::on(req), live.clone(), tty); // subscribe before spawning
    spawn_live(&a, &temp, &vols, live).await;
    hijack_response()
}

/// GET /exec/:id/json -- exec inspect (Running / ExitCode), how the CLI learns the exec's result.
pub(crate) async fn exec_inspect(State(a): State<App>, Path(id): Path<String>) -> Response {
    let g = a.inner.lock().await;
    let Some(exec) = g.execs.get(&id).cloned() else {
        return (StatusCode::NOT_FOUND, Json(json!({"message": format!("no such exec: {id}")}))).into_response();
    };
    let (running, code) = match g.live.get(&id) {
        Some(l) => match *l.exit_rx.borrow() { Some(c) => (false, c), None => (true, 0) },
        None => (false, 0),
    };
    Json(json!({"ID": id, "Running": running, "ExitCode": code, "ContainerID": exec.container_id,
        "ProcessConfig": {"tty": exec.tty, "entrypoint": exec.cmd.first().cloned().unwrap_or_default(),
            "arguments": exec.cmd.get(1..).map(|s| s.to_vec()).unwrap_or_default()}})).into_response()
}


// ---- container control: pause / rename / top / stats ------------------------
/// POST /containers/:id/(un)pause -- dd runs a container as one process group with no freezer cgroup;
/// accept and no-op so the CLI verbs succeed.
pub(crate) async fn containers_pause(State(a): State<App>, Path(id): Path<String>) -> Response { freeze(a, id, true).await }

pub(crate) async fn containers_unpause(State(a): State<App>, Path(id): Path<String>) -> Response { freeze(a, id, false).await }

/// docker pause/unpause. macOS has no freezer cgroup, but SIGSTOP/SIGCONT on the container's JIT process
/// freezes it (and its threads) just the same -- single-process / threaded containers (the common case)
/// freeze fully; a guest that forked separate host processes pauses its main process (best-effort).
pub(crate) async fn freeze(a: App, id: String, pause: bool) -> Response {
    let mut g = a.inner.lock().await;
    let Some(full) = resolve_cid(&g, &id) else { return no_such(&id); };
    if let Some(pid) = g.live.get(&full).and_then(|l| *l.pid.lock().unwrap()) {
        unsafe { libc::kill(pid as i32, if pause { libc::SIGSTOP } else { libc::SIGCONT }); }
    }
    if let Some(c) = g.containers.get_mut(&full) { c.status = if pause { "paused".into() } else { "running".into() }; }
    save_state(&g, &a.state_path);
    StatusCode::NO_CONTENT.into_response()
}

#[derive(Deserialize)]
pub(crate) struct RenameQ { name: Option<String> }

pub(crate) async fn containers_rename(State(a): State<App>, Path(id): Path<String>, Query(q): Query<RenameQ>) -> Response {
    let mut g = a.inner.lock().await;
    let Some(full) = resolve_cid(&g, &id) else { return no_such(&id) };
    if let Some(name) = q.name {
        if let Some(c) = g.containers.get_mut(&full) { c.name = name.trim_start_matches('/').to_string(); }
    }
    save_state(&g, &a.state_path);
    StatusCode::NO_CONTENT.into_response()
}

/// GET /containers/:id/top -- `docker top` (one synthetic process; dd doesn't expose a guest process tree).
pub(crate) async fn containers_top(State(a): State<App>, Path(id): Path<String>) -> Response {
    let g = a.inner.lock().await;
    let Some(full) = resolve_cid(&g, &id) else { return no_such(&id) };
    let cmd = g.containers.get(&full).map(|c| c.cmd.join(" ")).unwrap_or_default();
    Json(json!({ "Titles": ["UID", "PID", "PPID", "C", "STIME", "TTY", "TIME", "CMD"],
        "Processes": [["root", "1", "0", "0", "00:00", "?", "00:00:00", cmd]] })).into_response()
}

#[derive(Deserialize)]
pub(crate) struct StatsQ {
    /// `docker stats` streams by default (`stream=1`); `--no-stream` sends `stream=0`/`false`.
    stream: Option<String>,
}

/// Memory limit reported when the container set no `--memory` and we can't read host RAM (8 GiB).
const STATS_DEFAULT_LIMIT: u64 = 8 * 1024 * 1024 * 1024;
/// RSS fallback for a live pid whose `ps` lookup failed (so usage is never an implausible 0).
const STATS_MEM_FALLBACK: u64 = 8 * 1024 * 1024;
/// Synthetic CPU floor added per stream sample (30 ms) so a 1 s `system` delta yields ~3% even for an
/// idle guest -- the docker CLI then renders a sane non-zero %CPU. Real `ps` CPU time is added on top.
const STATS_CPU_FLOOR_NS: u64 = 30_000_000;

/// Best-effort (rss_bytes, cpu_nanos) for a host pid via `ps` (portable across Linux + macOS):
/// `ps -o rss=,time= -p <pid>` -> e.g. `" 12345 00:01:23"` (RSS in KiB, accumulated CPU time).
/// Returns (0, 0) if the pid is gone or `ps` can't be run.
fn pid_metrics(pid: u32) -> (u64, u64) {
    let out = std::process::Command::new("ps")
        .args(["-o", "rss=,time=", "-p", &pid.to_string()])
        .output();
    if let Ok(o) = out {
        if o.status.success() {
            let s = String::from_utf8_lossy(&o.stdout);
            let mut it = s.split_whitespace();
            let rss_kb = it.next().and_then(|v| v.parse::<u64>().ok()).unwrap_or(0);
            let cpu_ns = it.next().map(parse_ps_time).unwrap_or(0);
            return (rss_kb * 1024, cpu_ns);
        }
    }
    (0, 0)
}

/// Parse a `ps` accumulated-CPU-time field `"[[dd-]hh:]mm:ss[.frac]"` into nanoseconds.
fn parse_ps_time(s: &str) -> u64 {
    let (days, rest) = match s.split_once('-') {
        Some((d, r)) => (d.parse::<u64>().unwrap_or(0), r),
        None => (0, s),
    };
    // Fold the colon-separated h:m:s (or m:s) groups; drop any fractional seconds.
    let mut acc = 0u64;
    for p in rest.split(':') {
        let v = p.split('.').next().unwrap_or("0").parse::<u64>().unwrap_or(0);
        acc = acc * 60 + v;
    }
    (days * 86400 + acc) * 1_000_000_000
}

/// One `cpu_stats`/`precpu_stats` block in Docker's shape.
fn stats_cpu_block(total: u64, system: u64) -> Value {
    json!({
        "cpu_usage": { "total_usage": total, "usage_in_kernelmode": 0, "usage_in_usermode": total },
        "system_cpu_usage": system,
        "online_cpus": 1,
        "throttling_data": { "periods": 0, "throttled_periods": 0, "throttled_time": 0 }
    })
}

/// Build one full stats document. `base` anchors a monotonic `system_cpu_usage`; `idx` is the sample
/// number; `(pre_total, pre_sys)` is the previous sample's cpu totals (0/0 for the first sample, so the
/// CLI's first delta is the cumulative). Returns the JSON plus this sample's `(total, system)` so the
/// caller can thread them in as the next sample's precpu. A dead/absent pid yields an all-zero sample.
fn stats_sample(name: &str, id: &str, pid: Option<u32>, mem_limit: u64, idx: u64,
                base: std::time::Instant, pre_total: u64, pre_sys: u64) -> (Value, u64, u64) {
    let (total, system, mem, cur) = match pid {
        Some(p) => {
            let (rss, cpu) = pid_metrics(p);
            let mem = if rss == 0 { STATS_MEM_FALLBACK } else { rss };
            // system: monotonic host-clock proxy so the per-sample delta is real wall time.
            let system = 100_000_000_000u64 + base.elapsed().as_nanos() as u64;
            (cpu + idx * STATS_CPU_FLOOR_NS, system, mem, 1)
        }
        None => (0, 0, 0, 0),
    };
    let v = json!({
        "read": fmt_rfc3339(now_secs()),
        "name": format!("/{name}"),
        "id": id,
        "pids_stats": { "current": cur },
        "cpu_stats": stats_cpu_block(total, system),
        "precpu_stats": stats_cpu_block(pre_total, pre_sys),
        "memory_stats": { "usage": mem, "limit": mem_limit, "stats": {} },
        "blkio_stats": {
            "io_service_bytes_recursive": [], "io_serviced_recursive": [],
            "io_queue_recursive": [], "io_service_time_recursive": [],
            "io_wait_time_recursive": [], "io_merged_recursive": [],
            "io_time_recursive": [], "sectors_recursive": []
        },
        "networks": {}
    });
    (v, total, system)
}

/// GET /containers/:id/stats -- a Docker stats document. dd has no cgroup accounting, so metrics are
/// best-effort: memory + CPU come from the live JIT pid via `ps`, with a synthetic CPU floor so the CLI
/// shows a sane non-zero %. `stream=0`/`false` returns a single object; otherwise it's newline-delimited
/// JSON, one sample/sec, on a long-lived body that ends when the client disconnects (or a 1h cap).
pub(crate) async fn containers_stats(State(a): State<App>, Path(id): Path<String>, Query(q): Query<StatsQ>) -> Response {
    let (full, name, mem_limit, pid) = {
        let g = a.inner.lock().await;
        let Some(full) = resolve_cid(&g, &id) else { return no_such(&id) };
        let c = g.containers.get(&full);
        let name = c.map(|c| if c.name.is_empty() { c.id[..12.min(c.id.len())].to_string() } else { c.name.clone() })
            .unwrap_or_else(|| id.clone());
        let mem_limit = c.map(|c| c.memory).filter(|m| *m > 0).map(|m| m as u64).unwrap_or(STATS_DEFAULT_LIMIT);
        let pid = g.live.get(&full).and_then(|l| *l.pid.lock().unwrap());
        (full, name, mem_limit, pid)
    };
    let stream = !matches!(q.stream.as_deref(),
        Some("0") | Some("false") | Some("False") | Some("no") | Some("off"));

    // One-shot, or a container with no live process: emit a single sample (precpu = 0) and end.
    if !stream || pid.is_none() {
        let base = std::time::Instant::now();
        let (v, ..) = stats_sample(&name, &full, pid, mem_limit, 0, base, 0, 0);
        return Json(v).into_response();
    }

    // Live stream: re-sample once a second, threading each sample's cpu totals into the next precpu.
    // 3600 samples (~1h) is a safety cap; in practice the client disconnects and the stream is dropped.
    let base = std::time::Instant::now();
    let body = futures_util::stream::unfold((0u64, 0u64, 0u64), move |(idx, pre_total, pre_sys)| {
        let name = name.clone();
        let full = full.clone();
        async move {
            if idx >= 3600 { return None; }
            if idx > 0 { tokio::time::sleep(std::time::Duration::from_secs(1)).await; }
            let (v, total, system) = stats_sample(&name, &full, pid, mem_limit, idx, base, pre_total, pre_sys);
            let mut line = serde_json::to_vec(&v).unwrap_or_default();
            line.push(b'\n');
            Some((Ok::<Vec<u8>, std::io::Error>(line), (idx + 1, total, system)))
        }
    });
    Response::builder().status(StatusCode::OK).header("Content-Type", "application/json")
        .body(Body::from_stream(body)).unwrap()
}


#[derive(Deserialize)]
pub(crate) struct ResizeQ { h: Option<u16>, w: Option<u16> }

/// POST /containers/:id/resize and /exec/:id/resize -- set the PTY window size (TIOCSWINSZ) for a tty
/// container/exec. Always 200 so `docker run -t` never prints "failed to resize tty".
pub(crate) async fn resize(State(a): State<App>, Path(id): Path<String>, Query(q): Query<ResizeQ>) -> Response {
    let g = a.inner.lock().await;
    let key = resolve_cid(&g, &id).unwrap_or(id);
    if let Some(live) = g.live.get(&key) {
        if let Some(fd) = *live.pty_master.lock().unwrap() {
            let ws = libc::winsize { ws_row: q.h.unwrap_or(24), ws_col: q.w.unwrap_or(80), ws_xpixel: 0, ws_ypixel: 0 };
            unsafe { libc::ioctl(fd, libc::TIOCSWINSZ, &ws); }
        }
    }
    StatusCode::OK.into_response()
}


/// POST /containers/:id/wait -- block until the container exits, then return {"StatusCode": n}. CRITICAL:
/// the docker `run` CLI sends this BEFORE /start and reads it concurrently, so we must flush the response
/// HEADERS immediately (200) and stream the JSON body only once the guest exits -- otherwise the CLI
/// blocks waiting for the response and never sends /start (a deadlock).
pub(crate) async fn containers_wait(State(a): State<App>, Path(id): Path<String>) -> Response {
    let (full, live, done_code) = {
        let g = a.inner.lock().await;
        let Some(full) = resolve_cid(&g, &id) else { return no_such(&id) };
        let live = g.live.get(&full).cloned();
        let done = g.containers.get(&full).filter(|c| c.status == "exited").map(|c| c.exit_code);
        (full.clone(), live, done)
    };
    let stream = futures_util::stream::once(async move {
        let code = if let Some(c) = done_code {
            c
        } else if let Some(live) = live {
            let mut rx = live.exit_rx.clone();
            loop {
                let cur = *rx.borrow();
                if let Some(c) = cur { break c; }
                if rx.changed().await.is_err() { break 0; }
            }
        } else {
            0
        };
        let _ = full;
        Ok::<_, std::io::Error>(format!("{{\"StatusCode\":{code}}}\n").into_bytes())
    });
    Response::builder().status(StatusCode::OK).header("Content-Type", "application/json")
        .body(Body::from_stream(stream)).unwrap()
}

#[derive(Deserialize)]
pub(crate) struct LogsQ {
    /// `--tail`: "all" (or absent) for everything, otherwise the number of trailing lines.
    tail: Option<String>,
    /// `--timestamps`: prefix each line with an RFC3339 timestamp.
    timestamps: Option<String>,
    /// `--follow`: best-effort no-op here (dd containers run to completion; we return the buffer).
    follow: Option<String>,
    /// Stream selection. Docker requests at least one; default to both when neither is given.
    stdout: Option<String>,
    stderr: Option<String>,
}

fn q_truthy(s: &Option<String>) -> bool {
    matches!(s.as_deref(), Some("1") | Some("true") | Some("True"))
}

/// Split a log buffer into newline-terminated lines, keeping the trailing `\n` on each line and any
/// final unterminated fragment as its own line. Used to apply `--tail` and `--timestamps` per line.
fn split_log_lines(buf: &[u8]) -> Vec<Vec<u8>> {
    let mut lines = Vec::new();
    let mut start = 0;
    for (i, b) in buf.iter().enumerate() {
        if *b == b'\n' { lines.push(buf[start..=i].to_vec()); start = i + 1; }
    }
    if start < buf.len() { lines.push(buf[start..].to_vec()); }
    lines
}

pub(crate) async fn containers_logs(State(a): State<App>, Path(id): Path<String>, Query(q): Query<LogsQ>) -> Response {
    let g = a.inner.lock().await;
    let c = match resolve_cid(&g, &id).and_then(|f| g.containers.get(&f)) {
        Some(c) => c,
        None => return no_such(&id),
    };
    let _follow = q_truthy(&q.follow); // accepted; dd containers run to completion, so we serve the buffer
    let timestamps = q_truthy(&q.timestamps);
    // Stream selection: honor explicit stdout/stderr flags, defaulting to both when neither is given.
    let (mut want_out, mut want_err) = (q_truthy(&q.stdout), q_truthy(&q.stderr));
    if !want_out && !want_err { want_out = true; want_err = true; }
    // `--tail`: "all"/absent/unparsable -> everything; a number -> that many trailing lines.
    let tail = match q.tail.as_deref() {
        None | Some("") | Some("all") => None,
        Some(s) => s.parse::<usize>().ok(),
    };
    // Collect lines as (stream_id, bytes); stdout first then stderr, mirroring the previous ordering.
    let mut entries: Vec<(u8, Vec<u8>)> = Vec::new();
    if want_out { for line in split_log_lines(&c.stdout) { entries.push((1, line)); } }
    if want_err { for line in split_log_lines(&c.stderr) { entries.push((2, line)); } }
    if let Some(n) = tail { if entries.len() > n { entries.drain(0..entries.len() - n); } }
    // dd doesn't record per-line emit times; use the current time as a best-effort timestamp.
    let ts = fmt_rfc3339(now_secs());
    // TTY containers stream raw bytes (no demux header); non-TTY uses Docker's multiplexed framing
    // (8-byte header per chunk, stream id 1=stdout / 2=stderr). The timestamp, when requested, is part
    // of the stream payload so it survives demuxing -- exactly how dockerd writes it.
    let mut b = Vec::new();
    for (stream, line) in entries {
        let payload = if timestamps {
            let mut p = Vec::with_capacity(ts.len() + 1 + line.len());
            p.extend_from_slice(ts.as_bytes());
            p.push(b' ');
            p.extend_from_slice(&line);
            p
        } else { line };
        if c.tty { b.extend_from_slice(&payload); } else { b.extend(log_frame(stream, &payload)); }
    }
    b.into_response()
}

pub(crate) async fn containers_inspect(State(a): State<App>, Path(id): Path<String>) -> Response {
    let g = a.inner.lock().await;
    let Some(full) = resolve_cid(&g, &id) else { return no_such(&id) };
    match g.containers.get(&full) {
        Some(c) => {
            let running = c.status == "running" || c.status == "paused";
            // Pid = the live JIT process pid while running, else 0 (docker reports 0 for stopped).
            let pid = if running { g.live.get(&full).and_then(|l| *l.pid.lock().unwrap()).unwrap_or(0) } else { 0 };
            // StartedAt/FinishedAt as RFC3339; docker's zero value is "0001-01-01T00:00:00Z".
            let started_at = if c.started_at == 0 { "0001-01-01T00:00:00Z".to_string() } else { fmt_rfc3339(c.started_at) };
            let finished_at = if c.finished_at == 0 { "0001-01-01T00:00:00Z".to_string() } else { fmt_rfc3339(c.finished_at) };
            // Networks the container has joined -> NetworkSettings.Networks, with the IPAM-assigned
            // identity (IP/gateway/mac) per network.
            let networks: serde_json::Map<String, Value> = g.networks.iter()
                .filter_map(|n| n.endpoints.get(&c.id).map(|e| (n.name.clone(), json!({
                    "NetworkID": n.id, "IPAddress": e.ip, "Gateway": n.gateway,
                    "IPPrefixLen": n.subnet.split('/').nth(1).and_then(|p| p.parse::<i64>().ok()).unwrap_or(16),
                    "MacAddress": ip_mac(&e.ip)}))))
                .collect();
            // Top-level NetworkSettings.IPAddress = the primary endpoint IP (first joined network).
            let primary = g.networks.iter().find_map(|n| n.endpoints.get(&c.id).map(|e| (e.ip.clone(), n.gateway.clone()))).unwrap_or_default();
            Json(json!({"Id": c.id, "Image": c.image, "Created": fmt_rfc3339(c.created),
            "Name": format!("/{}", if c.name.is_empty() { c.id[..12.min(c.id.len())].to_string() } else { c.name.clone() }),
            "State": {"Status": c.status, "ExitCode": c.exit_code, "Running": running, "Paused": c.status == "paused",
                "Pid": pid, "StartedAt": started_at, "FinishedAt": finished_at},
            "Config": {"Cmd": c.cmd, "Hostname": c.hostname, "Image": c.image, "Env": c.env, "Labels": c.labels},
            "Mounts": c.binds.iter().filter_map(|b| b.split_once(':').map(|(s, d)| json!({"Source": s, "Destination": d, "Type": "bind"}))).collect::<Vec<_>>(),
            "HostConfig": {"Binds": c.binds, "Memory": c.memory, "PidsLimit": c.pids_limit},
            // NetworkSettings present so `docker port` (reads .NetworkSettings.Ports) doesn't panic.
            "NetworkSettings": {"Ports": ports_map_json(&c.publish), "IPAddress": primary.0, "Gateway": primary.1,
                "Networks": Value::Object(networks)}})).into_response()
        }
        None => no_such(&id) }
}

#[derive(Deserialize)]
pub(crate) struct PsQ { all: Option<String>, filters: Option<String> }

/// Apply `docker ps --filter`. `f` is the decoded `filters` map (`{"status":[..],"name":[..],"label":[..]}`).
/// Within a filter type the values are OR'd; `label` entries are AND'd (each must match). `name` is a
/// substring match against the container's effective name; `label` matches `key` or `key=value`.
fn ps_match(c: &Container, name: &str, f: &HashMap<String, Vec<String>>) -> bool {
    if let Some(vals) = f.get("status") { if !vals.iter().any(|v| v == &c.status) { return false; } }
    if let Some(vals) = f.get("name") { if !vals.iter().any(|v| name.contains(v.as_str())) { return false; } }
    if let Some(vals) = f.get("label") {
        for v in vals {
            let ok = match v.split_once('=') {
                Some((k, val)) => c.labels.get(k).map(|cv| cv == val).unwrap_or(false),
                None => c.labels.contains_key(v),
            };
            if !ok { return false; }
        }
    }
    true
}

/// Render a container's `docker ps` Status column the way docker does: "Up 3 minutes" while
/// running/paused, "Exited (0) 5 minutes ago" otherwise. The elapsed time is measured from the
/// container's `created` unix timestamp and humanized coarsely (seconds/minutes/hours/days).
fn human_status(c: &Container) -> String {
    let secs = (now_secs() - c.created).max(0);
    let dur = if secs < 60 { format!("{secs} seconds") }
        else if secs < 3600 { format!("{} minutes", secs / 60) }
        else if secs < 86400 { format!("{} hours", secs / 3600) }
        else { format!("{} days", secs / 86400) };
    if c.status == "running" || c.status == "paused" {
        format!("Up {dur}")
    } else {
        format!("Exited ({}) {dur} ago", c.exit_code)
    }
}

pub(crate) async fn containers_json(State(a): State<App>, Query(q): Query<PsQ>) -> Json<Value> {
    let all = matches!(q.all.as_deref(), Some("1") | Some("true") | Some("True"));
    // `filters` arrives URL-encoded JSON; axum has already percent-decoded it. Bad JSON => no filters.
    // Docker encodes it as map[key]->{value:true} (e.g. {"name":{"web":true}}), older clients as
    // map[key]->[value]. Accept BOTH: decode to a generic Value, normalize to key -> [values].
    let filters: HashMap<String, Vec<String>> = q.filters.as_deref()
        .and_then(|s| serde_json::from_str::<Value>(s).ok())
        .and_then(|v| v.as_object().map(|m| m.iter().map(|(k, val)| {
            let vals = match val {
                Value::Object(set) => set.keys().cloned().collect(),                 // {"web":true}
                Value::Array(a) => a.iter().filter_map(|x| x.as_str().map(String::from)).collect(), // ["web"]
                _ => vec![],
            };
            (k.clone(), vals)
        }).collect()))
        .unwrap_or_default();
    // A `status` filter implies "show all matching" (like `docker ps --filter status=exited`).
    let status_filter = filters.contains_key("status");
    let v: Vec<Value> = a.inner.lock().await.containers.values()
        .filter(|c| all || status_filter || c.status == "running")
        .filter(|c| {
            let name = if c.name.is_empty() { c.id[..12.min(c.id.len())].to_string() } else { c.name.clone() };
            ps_match(c, &name, &filters)
        })
        .map(|c| json!({
        "Id": c.id, "Image": c.image, "Command": c.cmd.join(" "), "Created": c.created,
        "State": c.status, "Status": human_status(c), "ExitCode": c.exit_code, "Ports": ports_json(&c.publish),
        "Labels": c.labels,
        "Mounts": c.binds.iter().filter_map(|b| b.split_once(':').map(|(s, d)| json!({"Source": s, "Destination": d, "Type": "bind"}))).collect::<Vec<_>>(),
        "Names": [format!("/{}", if c.name.is_empty() { c.id[..12.min(c.id.len())].to_string() } else { c.name.clone() })]})).collect();
    Json(json!(v))
}

pub(crate) async fn containers_delete(State(a): State<App>, Path(id): Path<String>) -> Response {
    let mut g = a.inner.lock().await;
    let full = match resolve_cid(&g, &id) { Some(f) => f, None => return no_such(&id) };
    if let Some(dc) = g.containers.remove(&full) {
        crate::events::emit_event(&a.events, "container", "destroy", &full, json!({"name": dc.name, "image": dc.image}));
        // Drop the container from any network membership too.
        for n in g.networks.iter_mut() { leave_network(n, &full); }
        save_state(&g, &a.state_path);
        StatusCode::NO_CONTENT.into_response()
    } else { no_such(&id) }
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

// ---- prune / changes / export / update (Docker conformance additions) -------

/// `POST /containers/prune` — `docker container prune`. Removes exited (non-running) containers and
/// reports what was deleted.
pub(crate) async fn containers_prune(State(a): State<App>) -> Json<Value> {
    let mut g = a.inner.lock().await;
    let dead: Vec<String> = g.containers.iter()
        .filter(|(_, c)| c.status != "running" && c.status != "paused")
        .map(|(id, _)| id.clone()).collect();
    for id in &dead { g.containers.remove(id); g.live.remove(id); }
    save_state(&g, &a.state_path);
    Json(json!({"ContainersDeleted": dead, "SpaceReclaimed": 0}))
}

/// `GET /containers/{id}/changes` — `docker diff`. dd does not track rootfs diffs; report no changes
/// (correct shape: an array of `{Path, Kind}`, or `null` for none).
pub(crate) async fn containers_changes(State(a): State<App>, Path(id): Path<String>) -> Response {
    let g = a.inner.lock().await;
    match resolve_cid(&g, &id) {
        Some(_) => Json(json!([])).into_response(),
        None => no_such(&id),
    }
}

/// `POST /containers/{id}/update` — `docker update`. dd does not apply live resource limits; accept
/// the request and return the conformant `{Warnings}` envelope.
pub(crate) async fn containers_update(State(a): State<App>, Path(id): Path<String>, _body: axum::body::Bytes) -> Response {
    let g = a.inner.lock().await;
    match resolve_cid(&g, &id) {
        Some(_) => Json(json!({"Warnings": []})).into_response(),
        None => no_such(&id),
    }
}

/// `GET /containers/{id}/export` — `docker export`. Streams a tar of the container rootfs.
pub(crate) async fn containers_export(State(a): State<App>, Path(id): Path<String>) -> Response {
    let rootfs = {
        let g = a.inner.lock().await;
        match resolve_cid(&g, &id).and_then(|f| g.containers.get(&f)).map(|c| c.rootfs.clone()) {
            Some(r) => r,
            None => return no_such(&id),
        }
    };
    match std::process::Command::new("tar").arg("cf").arg("-").arg("-C").arg(&rootfs).arg(".").output() {
        Ok(o) if o.status.success() => {
            Response::builder().status(StatusCode::OK).header("Content-Type", "application/x-tar")
                .body(Body::from(o.stdout)).unwrap()
        }
        _ => (StatusCode::INTERNAL_SERVER_ERROR, Json(json!({"message": "export failed"}))).into_response(),
    }
}

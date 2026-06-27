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
        network_mode: hc.as_ref().and_then(|h| h.network_mode.clone()).unwrap_or_default(),
        status: "created".into(), ..Default::default()
    };
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
        if let Some(cc) = g.containers.get_mut(&full) { cc.status = "running".into(); }
        (c, g.volumes.clone(), live)
    };
    if std::env::var("DD_DEBUG").is_ok() { eprintln!("[start] {} cmd={:?}", &c.id[..12], c.cmd); }
    spawn_live(&a, &c, &vols, live).await;
    StatusCode::NO_CONTENT.into_response()
}

/// stop / kill: containers run to completion synchronously, so there is no live process left to signal --
/// mark the container exited and return success (docker treats 204 as "stopped"). A long-running model
/// (background process + real signal delivery) is future work; the CLI verbs work today either way.
pub(crate) async fn containers_stop(State(a): State<App>, Path(id): Path<String>) -> Response {
    let mut g = a.inner.lock().await;
    match resolve_cid(&g, &id) {
        Some(full) => {
            if let Some(c) = g.containers.get_mut(&full) { c.status = "exited".into(); }
            save_state(&g, &a.state_path);
            StatusCode::NO_CONTENT.into_response()
        }
        None => no_such(&id),
    }
}

/// restart: just re-run the container in place.
pub(crate) async fn containers_restart(a: State<App>, id: Path<String>) -> Response { containers_start(a, id).await }


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
    g.execs.insert(exec_id.clone(), Exec { container_id: full, cmd, tty: body.tty.unwrap_or(false), started: false });
    (StatusCode::CREATED, Json(json!({"Id": exec_id}))).into_response()
}

/// POST /exec/:id/start -- run the exec command as a fresh JIT in the container's rootfs and stream its
/// IO over the hijacked connection (same path as attach).
pub(crate) async fn exec_start(State(a): State<App>, Path(id): Path<String>, req: Request) -> Response {
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
        let live = Live::new(exec.tty);
        g.live.insert(id.clone(), live.clone());
        if let Some(e) = g.execs.get_mut(&id) { e.started = true; }
        (temp, g.volumes.clone(), live, exec.tty)
    };
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

/// GET /containers/:id/stats -- one stats sample (dd has no cgroup accounting yet; zeros, valid shape).
pub(crate) async fn containers_stats(State(a): State<App>, Path(id): Path<String>) -> Response {
    let g = a.inner.lock().await;
    if resolve_cid(&g, &id).is_none() { return no_such(&id) }
    Json(json!({ "read": "1970-01-01T00:00:00Z", "name": format!("/{}", &id[..12.min(id.len())]),
        "cpu_stats": { "cpu_usage": { "total_usage": 0 }, "system_cpu_usage": 0, "online_cpus": 1 },
        "precpu_stats": { "cpu_usage": { "total_usage": 0 }, "system_cpu_usage": 0 },
        "memory_stats": { "usage": 0, "limit": 0 }, "pids_stats": { "current": 1 },
        "networks": {}, "blkio_stats": {} })).into_response()
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

pub(crate) async fn containers_logs(State(a): State<App>, Path(id): Path<String>) -> Response {
    let g = a.inner.lock().await;
    match resolve_cid(&g, &id).and_then(|f| g.containers.get(&f)) {
        // Docker's non-TTY log stream is multiplexed: 8-byte frame header per chunk. The docker
        // CLI demuxes it (and so does our dd-client).
        Some(c) => {
            let mut b = Vec::new();
            if !c.stdout.is_empty() { b.extend(log_frame(1, &c.stdout)); }
            if !c.stderr.is_empty() { b.extend(log_frame(2, &c.stderr)); }
            b.into_response()
        }
        None => no_such(&id),
    }
}

pub(crate) async fn containers_inspect(State(a): State<App>, Path(id): Path<String>) -> Response {
    let g = a.inner.lock().await;
    match resolve_cid(&g, &id).and_then(|f| g.containers.get(&f)) {
        Some(c) => {
            // Networks the container has joined -> NetworkSettings.Networks (membership reporting).
            let networks: serde_json::Map<String, Value> = g.networks.iter()
                .filter(|n| n.containers.iter().any(|x| x == &c.id))
                .map(|n| (n.name.clone(), json!({"NetworkID": n.id, "IPAddress": "", "Gateway": ""})))
                .collect();
            Json(json!({"Id": c.id, "Image": c.image, "Created": fmt_rfc3339(c.created),
            "Name": format!("/{}", if c.name.is_empty() { c.id[..12.min(c.id.len())].to_string() } else { c.name.clone() }),
            "State": {"Status": c.status, "ExitCode": c.exit_code, "Running": c.status == "running" || c.status == "paused", "Paused": c.status == "paused"},
            "Config": {"Cmd": c.cmd, "Hostname": c.hostname, "Image": c.image, "Env": c.env},
            "Mounts": c.binds.iter().filter_map(|b| b.split_once(':').map(|(s, d)| json!({"Source": s, "Destination": d, "Type": "bind"}))).collect::<Vec<_>>(),
            "HostConfig": {"Binds": c.binds, "Memory": c.memory, "PidsLimit": c.pids_limit},
            // NetworkSettings present so `docker port` (reads .NetworkSettings.Ports) doesn't panic.
            "NetworkSettings": {"Ports": ports_map_json(&c.publish), "IPAddress": "", "Gateway": "",
                "Networks": Value::Object(networks)}})).into_response()
        }
        None => no_such(&id) }
}

#[derive(Deserialize)]
pub(crate) struct PsQ { all: Option<String> }

pub(crate) async fn containers_json(State(a): State<App>, Query(q): Query<PsQ>) -> Json<Value> {
    let all = matches!(q.all.as_deref(), Some("1") | Some("true") | Some("True"));
    let v: Vec<Value> = a.inner.lock().await.containers.values()
        .filter(|c| all || c.status == "running")
        .map(|c| json!({
        "Id": c.id, "Image": c.image, "Command": c.cmd.join(" "), "Created": c.created,
        "State": c.status, "Status": c.status, "ExitCode": c.exit_code, "Ports": ports_json(&c.publish),
        "Mounts": c.binds.iter().filter_map(|b| b.split_once(':').map(|(s, d)| json!({"Source": s, "Destination": d, "Type": "bind"}))).collect::<Vec<_>>(),
        "Names": [format!("/{}", if c.name.is_empty() { c.id[..12.min(c.id.len())].to_string() } else { c.name.clone() })]})).collect();
    Json(json!(v))
}

pub(crate) async fn containers_delete(State(a): State<App>, Path(id): Path<String>) -> Response {
    let mut g = a.inner.lock().await;
    let full = match resolve_cid(&g, &id) { Some(f) => f, None => return no_such(&id) };
    if g.containers.remove(&full).is_some() {
        // Drop the container from any network membership too.
        for n in g.networks.iter_mut() { n.containers.retain(|c| c != &full); }
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

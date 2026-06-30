#![allow(unused_imports, dead_code)]
//! Hijacked-stream IO + exec handlers: attach, the docker `/exec` create/start/inspect
//! flow, and the shared PTY resize endpoint. Moved verbatim from the former
//! `containers.rs`; shared types/helpers come from `mod.rs` via `use super::*`.
use super::*;

/// POST /containers/:id/attach -- hijack the connection and stream the guest's IO. `docker run` (no -d)
/// and `docker run -it` use this: stdout/stderr come back framed (raw in TTY mode), and the client's
/// stdin (for -i) is fed to the guest. The hijacked stream closes when the guest exits.
/// Drive a hijacked docker stream against a Live: fan guest stdout/stderr to the client (docker
/// multiplexed frames, or raw bytes in TTY mode) and feed the client's stdin into the guest. Shared by
/// container attach and exec. `rx` is subscribed synchronously so no output is missed if the guest
/// starts producing before the upgrade completes.
pub(crate) fn spawn_hijack_io(on_upgrade: hyper::upgrade::OnUpgrade, live: Arc<Live>, tty: bool) {
    let mut rx = live.out.subscribe();
    // Close on `out_done` (set once the pumps have flushed ALL output), NOT on the immediate `exit`:
    // `exit` fires the instant the guest dies, while its final bytes may still be in-flight in the pump
    // tasks -- breaking on `exit` raced the pumps and dropped a fast-exiting command's last output.
    let mut out_done_rx = live.out_done_rx.clone();
    let live_in = live.clone();
    tokio::spawn(async move {
        let Ok(upgraded) = on_upgrade.await else { return };
        let (mut rd, mut wr) = tokio::io::split(TokioIo::new(upgraded));
        let writer = tokio::spawn(async move {
            // The guest may have already exited (and been fully drained) before the upgrade completed --
            // e.g. attaching to a retained, exited container -- so check the flag before blocking.
            let mut done = *out_done_rx.borrow();
            loop {
                if done {
                    // Output is complete: every byte is buffered in `out`. Flush it all, then end.
                    while let Ok((kind, chunk)) = rx.try_recv() {
                        let f = if tty { chunk } else { log_frame(kind, &chunk) };
                        let _ = wr.write_all(&f).await;
                    }
                    break;
                }
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
                    _ = out_done_rx.changed() => { done = true; }
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
    #[serde(rename = "Privileged")] privileged: Option<bool>,
}

/// POST /containers/:id/exec -- create an exec (record the command). Run it with /exec/:id/start.
pub(crate) async fn exec_create(State(a): State<App>, Path(id): Path<String>, Json(body): Json<ExecCreateBody>) -> Response {
    let mut g = a.inner.lock().await;
    let Some(full) = resolve_cid(&g, &id) else { return no_such(&id) };
    // `docker exec` into a non-running container is a 409 (docker rejects exec unless the container is
    // up). Match docker's message exactly so the CLI surfaces it verbatim.
    let running = g.containers.get(&full).map(|c| c.status == "running" || c.status == "paused").unwrap_or(false);
    if !running {
        return (StatusCode::CONFLICT, Json(json!({"message": format!("Container {full} is not running")}))).into_response();
    }
    let cmd = body.cmd.unwrap_or_default();
    if cmd.is_empty() {
        return (StatusCode::BAD_REQUEST, Json(json!({"message": "No exec command specified"}))).into_response();
    }
    let exec_id = new_id(&format!("exec-{full}"));
    g.execs.insert(exec_id.clone(), Exec { container_id: full, cmd, tty: body.tty.unwrap_or(false), started: false,
        env: body.env.unwrap_or_default(), working_dir: body.working_dir.unwrap_or_default(),
        user: body.user.unwrap_or_default(),
        // `--privileged`: metadata only (no Linux-cap enforcement in the JIT). Accept + record it so
        // exec inspect reflects it; the spawn path is unchanged (mirrors -e/-w/-u being plain fields).
        privileged: body.privileged.unwrap_or(false), exit_code: 0 });
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
        // `docker exec -e/-w/-u`: the exec inherits the container's env and adds `-e` overrides (later
        // wins), `-w` overrides the working dir, and `-u U[:G]` overrides the run user. spawn_cfg reads
        // temp.env / temp.working_dir / temp.user (-> DD_UID/DD_GID), so set them on the temp here.
        temp.env.extend(exec.env.iter().cloned());
        if !exec.working_dir.is_empty() { temp.working_dir = exec.working_dir.clone(); }
        if !exec.user.is_empty() { temp.user = exec.user.clone(); }
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
    // While the exec is live, read Running/ExitCode from its Live's exit watch. Once it exits the reaper
    // drops the Live (freeing its buffers) but records the code on the Exec, so fall back to that here.
    let (running, code) = match g.live.get(&id) {
        Some(l) => match *l.exit_rx.borrow() { Some(c) => (false, c), None => (true, 0) },
        None => (false, exec.exit_code),
    };
    Json(json!({"ID": id, "Running": running, "ExitCode": code, "ContainerID": exec.container_id,
        "ProcessConfig": {"tty": exec.tty, "privileged": exec.privileged,
            "entrypoint": exec.cmd.first().cloned().unwrap_or_default(),
            "arguments": exec.cmd.get(1..).map(|s| s.to_vec()).unwrap_or_default()}})).into_response()
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

#![allow(unused_imports, dead_code)]
//! Container lifecycle / control handlers: create, start, stop, kill, restart,
//! pause/unpause, rename, wait, delete. Moved verbatim from the former
//! `containers.rs`; shared helpers (parse_bind, parse_signal, do_stop, q_truthy)
//! live in `mod.rs` and are pulled in via `use super::*`.
use super::*;

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
    // `docker run --user U[:G]` — docker puts the "uid:gid" / "name" string in Config.User (top-level
    // of the create body, alongside Image/Cmd/Env). Stored on the Container and turned into DD_UID/DD_GID.
    #[serde(rename = "User")] user: Option<String>,
    #[serde(rename = "HostConfig")] host_config: Option<HostConfig>,
}

#[derive(Deserialize)]
pub(crate) struct HostConfig {
    #[serde(rename = "Binds")] binds: Option<Vec<String>>,
    #[serde(rename = "Memory")] memory: Option<i64>,
    #[serde(rename = "PidsLimit")] pids_limit: Option<i64>,
    #[serde(rename = "PortBindings")] port_bindings: Option<HashMap<String, Vec<PortBinding>>>,
    #[serde(rename = "NetworkMode")] network_mode: Option<String>,
    // HostConfig fidelity extras (parsed + persisted; round-tripped back through inspect).
    #[serde(rename = "RestartPolicy")] restart_policy: Option<RestartPolicy>,
    #[serde(rename = "CapAdd")] cap_add: Option<Vec<String>>,
    #[serde(rename = "CapDrop")] cap_drop: Option<Vec<String>>,
    #[serde(rename = "Devices")] devices: Option<Vec<DeviceMapping>>,
    #[serde(rename = "Mounts")] mounts: Option<Vec<Mount>>,
    #[serde(rename = "Privileged")] privileged: Option<bool>,
    // `--security-opt` (Vec<String> like ["sandbox"], ["seccomp=untrusted"], ["no-new-privileges"]).
    // Parsed + persisted verbatim; an entry matching sandbox/untrusted opts into the JIT sentry (spawn_cfg).
    #[serde(rename = "SecurityOpt")] security_opt: Option<Vec<String>>,
    // `--rm` (HostConfig.AutoRemove): the daemon removes the container automatically once it exits.
    #[serde(rename = "AutoRemove")] auto_remove: Option<bool>,
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
    // Match the image by name and, when --platform is given, by arch. A platform mismatch returns 404 so
    // the docker CLI pulls the right arch (its default --pull=missing won't re-pull otherwise) and retries.
    let want_arch = platform_arch(cq.platform.as_deref());
    // On a miss, re-scan the images dir from disk before giving up: the image may be on disk (freshly
    // pulled/built) yet absent from the in-memory store, which would otherwise force a spurious re-pull.
    {
        let g = a.inner.lock().await;
        let present = g.images.iter().filter(|i| ref_name(&i.name) == ref_name(&image))
            .any(|i| want_arch.map_or(true, |a| docker_arch(i.arch) == a));
        if !present { drop(g); rescan_images(&a).await; }
    }
    let mut g = a.inner.lock().await;
    // Restrict the store to the arch the user asked for (if any), then let `find_image` pick the single
    // best match deterministically (richest metadata wins; never an order-dependent duplicate).
    let candidates: Vec<Image> = g.images.iter()
        .filter(|i| want_arch.map_or(true, |a| docker_arch(i.arch) == a)).cloned().collect();
    let img = match find_image(&candidates, &image).cloned() {
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
    // `docker run --name X` with a name already in use is a 409 Conflict (docker refuses to start a
    // second container under the same name). Match on the effective name (leading `/` stripped, as we
    // store it). An empty name (no --name) never conflicts.
    let want_name = cq.name.as_deref().unwrap_or_default().trim_start_matches('/').to_string();
    if !want_name.is_empty() {
        if let Some(existing) = g.containers.values().find(|c| c.name == want_name) {
            return (StatusCode::CONFLICT, Json(json!({"message": format!(
                "Conflict. The container name \"/{want_name}\" is already in use by container \"{}\". \
                 You have to remove (or rename) that container to be able to reuse that name.", existing.id)}))).into_response();
        }
    }
    let id = new_id(&image);
    let hc = body.host_config;
    // Per-container copy-on-write upper layer over the read-only image rootfs (linux guests only; darwin
    // runs natively jailed and writes into its own rootfs). The guest's writes/creates/deletes land in
    // this private dir, so the shared image is never mutated. Reclaimed on `docker rm`/prune.
    let upper = if img.arch.os() == "darwin" {
        String::new()
    } else {
        let dir = dd_home().join("containers").join(&id).join("upper");
        let _ = std::fs::create_dir_all(&dir);
        dir.to_string_lossy().into_owned()
    };
    let c = Container {
        id: id.clone(), image, rootfs: img.rootfs, upper, cmd, arch: Some(img.arch),
        binds: hc.as_ref().and_then(|h| h.binds.clone()).unwrap_or_default(),
        hostname: body.hostname.unwrap_or_default(),
        memory: hc.as_ref().and_then(|h| h.memory).unwrap_or(0),
        pids_limit: hc.as_ref().and_then(|h| h.pids_limit).unwrap_or(0),
        publish: hc.as_ref().and_then(|h| h.port_bindings.as_ref()).map(publish_str).unwrap_or_default(),
        created: now_secs(), tty,
        name: want_name,
        working_dir, env,
        user: body.user.unwrap_or_default(),
        labels: body.labels.unwrap_or_default(),
        network_mode: hc.as_ref().and_then(|h| h.network_mode.clone()).unwrap_or_default(),
        // HostConfig fidelity extras: parse + persist verbatim (surfaced back in inspect HostConfig).
        // `--mount` entries (bind/volume) are additionally wired into the rootfs in spawn_cfg via the
        // same Volume mechanism as `-v`/Binds. CapAdd/CapDrop/Devices/Privileged are metadata (the JIT
        // doesn't enforce Linux capabilities/devices); RestartPolicy drives the spawn-time supervisor.
        restart_policy: hc.as_ref().and_then(|h| h.restart_policy.clone()).unwrap_or_default(),
        cap_add: hc.as_ref().and_then(|h| h.cap_add.clone()).unwrap_or_default(),
        cap_drop: hc.as_ref().and_then(|h| h.cap_drop.clone()).unwrap_or_default(),
        devices: hc.as_ref().and_then(|h| h.devices.clone()).unwrap_or_default(),
        mounts: hc.as_ref().and_then(|h| h.mounts.clone()).unwrap_or_default(),
        privileged: hc.as_ref().and_then(|h| h.privileged).unwrap_or(false),
        security_opt: hc.as_ref().and_then(|h| h.security_opt.clone()).unwrap_or_default(),
        auto_remove: hc.as_ref().and_then(|h| h.auto_remove).unwrap_or(false),
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
    if let Some(l) = g.live.get(&full) {
        l.stop_requested.store(true, std::sync::atomic::Ordering::SeqCst); // deliberate stop: no auto-restart
        if let Some(pid) = *l.pid.lock().unwrap() { unsafe { libc::kill(pid as i32, sig); } } // mirror of freeze()'s libc::kill
    }
    if let Some(c) = g.containers.get_mut(&full) { c.status = "exited".into(); c.finished_at = now_secs(); }
    let (cname, cimage) = g.containers.get(&full).map(|c| (c.name.clone(), c.image.clone())).unwrap_or_default();
    crate::events::emit_event(&a.events, "container", "kill", &full, json!({"name": cname, "image": cimage}));
    save_state(&g, &a.state_path);
    StatusCode::NO_CONTENT.into_response()
}

/// restart: stop the live process (real signal, via the stop path) then spawn a FRESH `Live` so the
/// guest truly re-runs. We can't reuse `containers_start` here: its `g.live.entry(..).or_insert_with`
/// would return the OLD, spent `Live` (whose `started` flag is already set), and `spawn_live` no-ops on
/// an already-started `Live` — so the container would never actually re-spawn. `do_stop` set
/// `stop_requested` on that old `Live`, so when its process dies the RestartPolicy supervisor skips it
/// (a deliberate `docker restart` must not be double-counted as a crash); this handler owns the respawn.
/// The new `Live` starts with `stop_requested=false`, so a *future* crash still follows `--restart`.
pub(crate) async fn containers_restart(State(a): State<App>, Path(id): Path<String>, Query(q): Query<StopQ>) -> Response {
    let sig = q.signal.as_deref().map(|s| parse_signal(s, libc::SIGTERM)).unwrap_or(libc::SIGTERM);
    let t = q.t.unwrap_or(10).max(0);
    // Stop the running process (if any). `do_stop` blocks until the old reaper flips status to "exited"
    // (or the container had no live process), so its state writes are done before we install the new Live.
    let _ = do_stop(&a, &id, sig, t).await;
    let (c, vols, live) = {
        let mut g = a.inner.lock().await;
        let full = match resolve_cid(&g, &id) { Some(f) => f, None => return no_such(&id) };
        let c = match g.containers.get(&full).cloned() { Some(c) => c, None => return no_such(&id) };
        // Replace the spent Live with a fresh one (mirrors maybe_restart / start's spawn).
        let live = Live::new(c.tty);
        g.live.insert(full.clone(), live.clone());
        if let Some(cc) = g.containers.get_mut(&full) { cc.status = "running".into(); cc.started_at = now_secs(); }
        (c, g.volumes.clone(), live)
    };
    if std::env::var("DD_DEBUG").is_ok() { eprintln!("[restart] {} cmd={:?}", &c.id[..12], c.cmd); }
    spawn_live(&a, &c, &vols, live).await;
    crate::events::emit_event(&a.events, "container", "start", &c.id, json!({"name": c.name, "image": c.image}));
    crate::events::emit_event(&a.events, "container", "restart", &c.id, json!({"name": c.name}));
    StatusCode::NO_CONTENT.into_response()
}

// ---- container control: pause / unpause / rename ----------------------------
/// POST /containers/:id/(un)pause -- dd has no freezer cgroup, so it SIGSTOP/SIGCONTs the container's
/// whole process group (see `freeze`) and flips the recorded status.
pub(crate) async fn containers_pause(State(a): State<App>, Path(id): Path<String>) -> Response { freeze(a, id, true).await }

pub(crate) async fn containers_unpause(State(a): State<App>, Path(id): Path<String>) -> Response { freeze(a, id, false).await }

/// docker pause/unpause. macOS has no freezer cgroup, but the container runs in its own process group
/// (the JIT is the group leader; host processes the guest forks inherit that pgid -- see spawn_live), so
/// a single SIGSTOP/SIGCONT to the GROUP freezes/resumes the WHOLE container -- the main process AND any
/// forked children -- not just the leader. We signal the group via killpg (`kill(-pgid)`) and, only if
/// that fails (e.g. the leader is mid-teardown), fall back to the leader pid alone.
pub(crate) async fn freeze(a: App, id: String, pause: bool) -> Response {
    let mut g = a.inner.lock().await;
    let Some(full) = resolve_cid(&g, &id) else { return no_such(&id); };
    if let Some(pid) = g.live.get(&full).and_then(|l| *l.pid.lock().unwrap()) {
        let pid = pid as i32;
        let sig = if pause { libc::SIGSTOP } else { libc::SIGCONT };
        // pid is the group leader, so -pid is the container's process group id (pgid == leader pid).
        unsafe { if libc::kill(-pid, sig) != 0 { libc::kill(pid, sig); } }
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
pub(crate) struct DeleteQ { force: Option<String>, v: Option<String>, link: Option<String> }

pub(crate) async fn containers_delete(State(a): State<App>, Path(id): Path<String>, Query(q): Query<DeleteQ>) -> Response {
    let force = q_truthy(&q.force);
    let mut g = a.inner.lock().await;
    let full = match resolve_cid(&g, &id) { Some(f) => f, None => return no_such(&id) };
    // `docker rm` of a running container without `-f` is a 409: docker refuses to remove a live
    // container and tells the user to stop it (or use `--force`). With `--force` we stop it first.
    let running = g.containers.get(&full).map(|c| c.status == "running" || c.status == "paused").unwrap_or(false);
    if running && !force {
        let short = &full[..12.min(full.len())];
        return (StatusCode::CONFLICT, Json(json!({"message": format!(
            "cannot remove a running container {short}: Stop the container before removing or force remove")}))).into_response();
    }
    // Removing a container cancels any pending RestartPolicy restart; with `--force` on a running
    // container we also SIGKILL the live process so the reaper doesn't resurrect/dangle it.
    if let Some(l) = g.live.get(&full) {
        l.stop_requested.store(true, std::sync::atomic::Ordering::SeqCst);
        if force && running { if let Some(pid) = *l.pid.lock().unwrap() { unsafe { libc::kill(pid as i32, libc::SIGKILL); } } }
    }
    if let Some(dc) = g.containers.remove(&full) {
        crate::events::emit_event(&a.events, "container", "destroy", &full, json!({"name": dc.name, "image": dc.image}));
        // Drop the container from any network membership too.
        for n in g.networks.iter_mut() { leave_network(n, &full); }
        // Reclaim the container's private writable upper layer (Docker discards the writable layer on rm).
        // The shared image rootfs (the read-only lower) is never touched. Also drop its live IO plumbing
        // (log buffers + channels); otherwise `docker rm` leaks them.
        discard_container_layer(&dc.upper);
        g.live.remove(&full);
        save_state(&g, &a.state_path);
        StatusCode::NO_CONTENT.into_response()
    } else { no_such(&id) }
}

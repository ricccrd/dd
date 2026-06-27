//! dd-daemon — a Docker-Engine-API daemon backed by the **dd** VM-less JIT runtime.
//!
//! The real `docker` CLI (and the `dd-app` GUI) talk to this over a Unix socket; container
//! *execution* is delegated to the JIT binaries built by the `ddjit` crate (one per guest
//! architecture). The daemon detects each image's architecture from its ELF and picks the
//! matching JIT, then launches it via the typed [`ddjit::SpawnConfig`] contract — no VM.
//!
//!   cargo run --release -p dd-daemon            # build.rs builds the JITs first
//!   DOCKER_HOST=unix://$PWD/dd.sock docker run -p 8080:80 -m 256m alpine echo hi
//!
//! Containers, volumes and networks are persisted to `DD_STATE` (default `~/.dd/state.json`) so
//! they survive daemon restarts. Images are re-discovered from `DD_IMAGES` each startup.
//!
//! Env: DD_IMAGES (image dirs; default "./images"), DDOCKERD_SOCK (listen socket),
//!      DD_STATE (state file), DD_VOLUMES (named-volume root).

use axum::{
    body::Body,
    extract::{Path, Query, Request, State},
    http::{StatusCode, Uri},
    response::{IntoResponse, Response},
    routing::{delete, get, post},
    Json, Router,
};
use ddjit::{Guest, PortMap, SpawnConfig, Volume};
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

mod registry;
use registry::{Client, Credentials, ImageRef};

const API_VERSION: &str = "1.43";

#[derive(Clone, Default)]
struct Image {
    name: String,
    rootfs: String,
    arch: Guest,
    cmd: Vec<String>,
    // the rest of the OCI/Dockerfile config metadata a container inherits at run
    env: Vec<String>,        // "K=V" entries (ENV)
    entrypoint: Vec<String>, // ENTRYPOINT (prepended to the command)
    workdir: String,         // WORKDIR / Config.WorkingDir
}

#[derive(Clone, Default, Serialize, Deserialize)]
struct Container {
    id: String,
    image: String,
    rootfs: String,
    cmd: Vec<String>,
    binds: Vec<String>,
    hostname: String,
    memory: i64,
    pids_limit: i64,
    publish: String,
    created: i64,
    status: String,
    exit_code: i64,
    #[serde(default)]
    tty: bool,
    #[serde(default)]
    name: String,
    #[serde(default)]
    working_dir: String,
    #[serde(default)]
    env: Vec<String>, // "K=V" entries from the image ENV + `docker run -e`
    #[serde(default)]
    network_mode: String,
    // Re-derived from the image at load; never serialized.
    #[serde(skip)]
    arch: Option<Guest>,
    // Captured output is not persisted (would bloat the state file).
    #[serde(skip)]
    stdout: Vec<u8>,
    #[serde(skip)]
    stderr: Vec<u8>,
}

/// A running container's live IO plumbing. Created on first attach-or-start, dropped when the guest
/// process exits. The process stdout/stderr fan out to (a) any attached clients via `out`, (b) the log
/// buffers for `docker logs`. `stdin` feeds the guest for `-i`/attach.
struct Live {
    out: broadcast::Sender<(u8, Vec<u8>)>, // (1=stdout, 2=stderr, chunk)
    stdin_tx: mpsc::Sender<Vec<u8>>,        // attach writes here; an empty Vec = stdin EOF
    stdin_rx: Mutex<Option<mpsc::Receiver<Vec<u8>>>>, // start() takes it and feeds the guest
    stdout_buf: Mutex<Vec<u8>>,
    stderr_buf: Mutex<Vec<u8>>,
    exit: watch::Sender<Option<i64>>, // Some(code) once exited
    exit_rx: watch::Receiver<Option<i64>>,
    started: std::sync::atomic::AtomicBool, // start() spawns the process exactly once
    tty: bool,
    pty_master: std::sync::Mutex<Option<RawFd>>, // the PTY master fd (tty containers) for /resize
    pid: std::sync::Mutex<Option<u32>>,          // the live JIT process pid (for pause = SIGSTOP/SIGCONT)
}
impl Live {
    fn new(tty: bool) -> Arc<Self> {
        let (out, _) = broadcast::channel(1024);
        let (exit, exit_rx) = watch::channel(None);
        let (stdin_tx, stdin_rx) = mpsc::channel(256);
        Arc::new(Live { out, stdin_tx, stdin_rx: Mutex::new(Some(stdin_rx)), stdout_buf: Mutex::new(Vec::new()),
            stderr_buf: Mutex::new(Vec::new()), exit, exit_rx,
            started: std::sync::atomic::AtomicBool::new(false), tty,
            pty_master: std::sync::Mutex::new(None), pid: std::sync::Mutex::new(None) })
    }
}

/// A named volume — a directory under the volumes root that containers can bind by name.
#[derive(Clone, Serialize, Deserialize)]
struct Vol {
    name: String,
    mountpoint: String,
    created_at: i64,
}

/// A user-defined network. dd's isolation is a per-container loopback netns (see `run_in_jit`);
/// a network here is metadata plus the set of attached containers.
#[derive(Clone, Serialize, Deserialize)]
struct Net {
    id: String,
    name: String,
    driver: String,
    scope: String,
    #[serde(default)]
    containers: Vec<String>,
    #[serde(default)]
    created: i64,
}

/// A `docker exec` invocation: a command to run in a container's rootfs. dd runs it as a fresh JIT
/// process sharing the container's rootfs + volumes (the same files; a distinct process namespace).
#[derive(Clone)]
struct Exec {
    container_id: String,
    cmd: Vec<String>,
    tty: bool,
    started: bool,
}

#[derive(Default)]
struct Inner {
    containers: HashMap<String, Container>,
    images: Vec<Image>,
    volumes: Vec<Vol>,
    networks: Vec<Net>,
    live: HashMap<String, Arc<Live>>, // running containers' (and execs') IO plumbing (not persisted)
    execs: HashMap<String, Exec>,     // exec id -> its spec
}

/// The serializable slice of [`Inner`] written to `DD_STATE`.
#[derive(Default, Serialize, Deserialize)]
struct Persisted {
    containers: Vec<Container>,
    volumes: Vec<Vol>,
    networks: Vec<Net>,
}

#[derive(Clone)]
struct App {
    inner: Arc<Mutex<Inner>>,
    state_path: String,
    volumes_dir: String,
    images_dir: String,
}

#[tokio::main]
async fn main() {
    let images_dir = std::env::var("DD_IMAGES").unwrap_or_else(|_| "./images".into());
    let sock = std::env::var("DDOCKERD_SOCK").unwrap_or_else(|_| "./dd.sock".into());
    let state_path = std::env::var("DD_STATE").unwrap_or_else(|_| dd_home().join("state.json").to_string_lossy().into_owned());
    let volumes_dir = std::env::var("DD_VOLUMES").unwrap_or_else(|_| dd_home().join("volumes").to_string_lossy().into_owned());
    let _ = std::fs::remove_file(&sock);
    let _ = std::fs::create_dir_all(&volumes_dir);

    let mut inner = Inner::default();
    inner.images = discover_images(&images_dir);
    load_state(&mut inner, &state_path);
    if inner.networks.is_empty() {
        inner.networks = default_networks();
    }
    eprintln!(
        "[dd-daemon] images={} -> {} image(s): {}",
        images_dir,
        inner.images.len(),
        inner.images.iter().map(|i| format!("{}({})", i.name, i.arch.target())).collect::<Vec<_>>().join(", ")
    );
    eprintln!(
        "[dd-daemon] state={state_path} -> {} container(s), {} volume(s), {} network(s)",
        inner.containers.len(), inner.volumes.len(), inner.networks.len()
    );
    for g in Guest::ALL {
        eprintln!("[dd-daemon] JIT {}: {}", g.target(), if ddjit::available(g) { "ready" } else { "NOT BUILT" });
    }
    let app = App { inner: Arc::new(Mutex::new(inner)), state_path, volumes_dir, images_dir };

    let router = Router::new()
        .route("/_ping", get(|| async { "OK" }))
        .route("/version", get(version)).route("/info", get(info))
        .route("/images/json", get(images_json)).route("/images/create", post(images_create))
        .route("/images/:name/json", get(image_inspect))
        .route("/images/:name/push", post(image_push))
        .route("/build", post(images_build))
        .route("/images/:name/tag", post(image_tag))
        .route("/images/:name", delete(image_delete))
        .route("/containers/json", get(containers_json))
        .route("/containers/create", post(containers_create))
        .route("/containers/:id/start", post(containers_start))
        .route("/containers/:id/attach", post(containers_attach))
        .route("/containers/:id/stop", post(containers_stop))
        .route("/containers/:id/kill", post(containers_stop))
        .route("/containers/:id/restart", post(containers_restart))
        .route("/containers/:id/pause", post(containers_pause))
        .route("/containers/:id/unpause", post(containers_unpause))
        .route("/containers/:id/rename", post(containers_rename))
        .route("/containers/:id/top", get(containers_top))
        .route("/containers/:id/stats", get(containers_stats))
        .route("/containers/:id/wait", post(containers_wait))
        .route("/containers/:id/resize", post(resize))
        .route("/containers/:id/logs", get(containers_logs))
        .route("/containers/:id/json", get(containers_inspect))
        .route("/containers/:id/archive", get(archive_get).put(archive_put).head(archive_head))
        .route("/containers/:id/exec", post(exec_create))
        .route("/exec/:id/start", post(exec_start))
        .route("/exec/:id/resize", post(resize))
        .route("/exec/:id/json", get(exec_inspect))
        .route("/containers/:id", delete(containers_delete))
        .route("/volumes", get(volumes_list))
        .route("/volumes/create", post(volumes_create))
        .route("/volumes/:name", get(volume_inspect).delete(volume_delete))
        .route("/networks", get(networks_list))
        .route("/networks/create", post(networks_create))
        .route("/networks/:id", get(network_inspect).delete(network_delete))
        .route("/networks/:id/connect", post(network_connect))
        .route("/networks/:id/disconnect", post(network_disconnect))
        .fallback(not_found).with_state(app);

    let listener = tokio::net::UnixListener::bind(&sock).expect("bind unix socket");
    eprintln!("[dd-daemon] listening on unix://{sock}");
    let mut make = router.into_make_service();
    loop {
        let (socket, _) = match listener.accept().await { Ok(x) => x, Err(_) => continue };
        let svc = tower::Service::call(&mut make, &socket).await.unwrap();
        tokio::spawn(async move {
            let io = hyper_util::rt::TokioIo::new(socket);
            let hsvc = hyper::service::service_fn(move |mut req: hyper::Request<hyper::body::Incoming>| {
                strip_api_version(&mut req);
                if std::env::var("DD_DEBUG").is_ok() { eprintln!("[req] {} {}", req.method(), req.uri().path()); }
                tower::ServiceExt::oneshot(svc.clone(), req)
            });
            // serve_connection_with_upgrades: required for the attach/exec hijack (HTTP Upgrade: tcp).
            let _ = hyper_util::server::conn::auto::Builder::new(hyper_util::rt::TokioExecutor::new())
                .serve_connection_with_upgrades(io, hsvc).await;
        });
    }
}

fn strip_api_version<B>(req: &mut hyper::Request<B>) {
    let mut pq = req.uri().path_and_query().map(|p| p.as_str().to_string()).unwrap_or_default();
    // strip the /v1.NN API-version prefix
    if let Some(rest) = pq.strip_prefix("/v1.") {
        if let Some(slash) = rest.find('/') { pq = rest[slash..].to_string(); }
    }
    // collapse a multi-segment image reference to its bare name so the :name routes match. docker sends
    // the canonical path, e.g. POST /images/docker.io/library/ubuntu/push -> /images/ubuntu/push.
    pq = normalize_image_path(&pq);
    if let Ok(uri) = pq.parse::<Uri>() { *req.uri_mut() = uri; }
}
/// `/images/<registry>/<ns>/<name>/<verb>` -> `/images/<name>/<verb>` (verb = push|tag|json); other
/// paths pass through unchanged. The handlers further ref_name() the captured segment.
fn normalize_image_path(pq: &str) -> String {
    let (path, query) = pq.split_once('?').map(|(p, q)| (p, Some(q))).unwrap_or((pq, None));
    let rebuild = |p: String| match query { Some(q) => format!("{p}?{q}"), None => p };
    let Some(rest) = path.strip_prefix("/images/") else { return pq.to_string() };
    let segs: Vec<&str> = rest.split('/').collect();
    if segs.len() <= 2 { return pq.to_string(); } // already /images/<name>[/<verb>]
    if let Some(verb @ ("push" | "tag" | "json")) = segs.last().copied() {
        let name = segs[segs.len() - 2]; // bare image name sits right before the verb
        return rebuild(format!("/images/{name}/{verb}"));
    }
    pq.to_string()
}

async fn not_found(uri: Uri) -> Response {
    (StatusCode::NOT_FOUND, Json(json!({"message": format!("no route for {uri}")}))).into_response()
}
async fn version() -> Json<Value> {
    Json(json!({"Version": "0.1.0-dd", "ApiVersion": API_VERSION, "MinAPIVersion": "1.24",
        "Os": "linux", "Arch": "arm64", "KernelVersion": "6.1.0-dd",
        "Components": [{"Name": "Engine", "Version": "0.1.0-dd"}]}))
}
async fn info(State(a): State<App>) -> Json<Value> {
    let g = a.inner.lock().await;
    Json(json!({"ID": "DD", "Containers": g.containers.len(), "Images": g.images.len(),
        "Volumes": g.volumes.len(), "Networks": g.networks.len(),
        "Driver": "jit-overlay", "OperatingSystem": "dd (VM-less JIT on macOS)",
        "OSType": "linux", "Architecture": "aarch64", "NCPU": 1, "ServerVersion": "0.1.0-dd"}))
}
async fn images_json(State(a): State<App>) -> Json<Value> {
    let imgs: Vec<Value> = a.inner.lock().await.images.iter().map(|i| json!({
        "Id": format!("sha256:{}", fake_id(&i.name)), "RepoTags": [repo_tag(&i.name)],
        "Architecture": i.arch.target(), "Created": 0, "Size": image_size(&i.rootfs, &i.name)})).collect();
    Json(json!(imgs))
}
/// GET /images/:name/json — `docker image inspect` / `docker run`'s local-image probe. Returns the
/// image config (Cmd/Entrypoint/Env) so the CLI doesn't treat the image as missing and re-pull.
async fn image_inspect(State(a): State<App>, Path(name): Path<String>) -> Response {
    let g = a.inner.lock().await;
    match g.images.iter().find(|i| ref_name(&i.name) == ref_name(&name)) {
        Some(i) => Json(json!({
            "Id": format!("sha256:{}", fake_id(&i.name)),
            "RepoTags": [repo_tag(&i.name)], "RepoDigests": [],
            "Architecture": docker_arch(i.arch),
            "Os": i.arch.os(), "Size": 0, "Created": "1970-01-01T00:00:00Z",
            "Config": {"Cmd": i.cmd, "Entrypoint": Value::Null, "Env": [
                "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"], "WorkingDir": ""},
            "RootFS": {"Type": "layers", "Layers": []}})).into_response(),
        None => (StatusCode::NOT_FOUND, Json(json!({"message": format!("No such image: {name}")}))).into_response(),
    }
}
#[derive(Deserialize)]
struct ImageCreateQ {
    #[serde(rename = "fromImage")] from_image: Option<String>,
    #[serde(rename = "tag")] tag: Option<String>,
    platform: Option<String>,
}
/// POST /images/create -- `docker pull`. If the image isn't local (for the requested platform), pull it
/// from its registry (any registry) and unpack it into a rootfs, then register it.
async fn images_create(State(a): State<App>, Query(q): Query<ImageCreateQ>, headers: axum::http::HeaderMap) -> Response {
    let name = q.from_image.unwrap_or_default();
    let tag = q.tag.filter(|t| !t.is_empty()).unwrap_or_else(|| "latest".into());
    if name.is_empty() { return (StatusCode::BAD_REQUEST, Json(json!({"message": "fromImage is required"}))).into_response(); }
    // "already local" must match the FULL reference (registry/repo:tag) AND the requested --platform arch:
    // distinct images can share a short name across registries, and arm64/amd64 of one image are distinct.
    let want = image_ref(&name, &tag).short();
    let want_arch = platform_arch(q.platform.as_deref());
    if a.inner.lock().await.images.iter()
        .any(|i| repo_tag(&i.name) == want && want_arch.map_or(true, |a| docker_arch(i.arch) == a))
    {
        return pull_progress(&name, &tag, Ok(true));
    }
    let creds = registry_auth(&headers);
    let archs = platform_archs(q.platform.as_deref());
    let (dir, nm, tg) = (a.images_dir.clone(), name.clone(), tag.clone());
    let res = tokio::task::spawn_blocking(move || pull_image(&dir, &nm, &tg, creds, &archs)).await
        .unwrap_or_else(|e| Err(format!("pull task crashed: {e}")));
    match res {
        Ok(img) => {
            let mut g = a.inner.lock().await;
            g.images.retain(|i| repo_tag(&i.name) != want); // a re-pull (e.g. a new platform) replaces the old
            g.images.push(img);
            pull_progress(&name, &tag, Ok(false))
        }
        Err(e) => pull_progress(&name, &tag, Err(e)),
    }
}

/// Decode the CLI's `X-Registry-Auth` header (base64 JSON credentials) into [`Credentials`].
fn registry_auth(headers: &axum::http::HeaderMap) -> Credentials {
    headers.get("X-Registry-Auth").and_then(|v| v.to_str().ok())
        .and_then(Credentials::from_x_registry_auth).unwrap_or_default()
}

/// docker-style pull progress: a stream of JSON status lines the CLI renders.
fn pull_progress(name: &str, tag: &str, result: Result<bool, String>) -> Response {
    let body = match result {
        Ok(true) => format!("{}\r\n", json!({ "status": format!("Status: Image is up to date for {name}:{tag}") })),
        Ok(false) => [
            json!({ "status": format!("Pulling from {}", image_ref(name, tag).repository) }).to_string(),
            json!({ "status": "Verifying Checksum" }).to_string(),
            json!({ "status": "Download complete" }).to_string(),
            json!({ "status": "Pull complete" }).to_string(),
            json!({ "status": format!("Status: Downloaded newer image for {name}:{tag}") }).to_string(),
        ].join("\r\n") + "\r\n",
        Err(e) => json!({ "errorDetail": { "message": e.clone() }, "error": e }).to_string() + "\r\n",
    };
    (StatusCode::OK, [("Content-Type", "application/json")], body).into_response()
}

/// Build an [`ImageRef`] from docker's separate `fromImage` + `tag` params (tag overrides any in the ref).
fn image_ref(from_image: &str, tag: &str) -> ImageRef {
    let mut r = ImageRef::parse(from_image);
    if !tag.is_empty() { r.tag = tag.to_string(); }
    r
}

/// Pull an image from its registry (any registry) and unpack it under `<images_dir>/<safe>/rootfs`,
/// preferring the linux/arm64 variant (native; falls back to amd64). Returns the registered [`Image`].
fn pull_image(images_dir: &str, from_image: &str, tag: &str, creds: Credentials, archs: &[&str]) -> Result<Image, String> {
    let iref = image_ref(from_image, tag);
    let rootfs = std::path::PathBuf::from(format!("{images_dir}/{}/rootfs", safe_name(&iref)));
    let pulled = Client::new(iref.clone(), creds).pull(&rootfs, archs)?;
    let arch = detect_arch(&rootfs).ok_or("could not detect the image architecture")?;
    let cmd = config_cmd(&pulled.config).unwrap_or_else(|| default_shell(&rootfs));
    let name = iref.short();
    // Record name + cmd so the image keeps its real identity across a daemon restart (the dir name
    // alone doesn't round-trip -- e.g. "docker.io_library_alpine_latest").
    let _ = std::fs::write(format!("{images_dir}/{}/dd-image.json", safe_name(&iref)),
        json!({ "name": name.clone(), "cmd": cmd.clone() }).to_string());
    Ok(Image { name, rootfs: rootfs.to_string_lossy().into_owned(), arch, cmd, ..Default::default() })
}

/// A `repository:tag` string with exactly one tag — discovered images carry a bare name (`busybox`),
/// pulled ones already include the tag (`busybox:latest`); append `:latest` only when absent.
fn repo_tag(name: &str) -> String {
    let last = name.rsplit('/').next().unwrap_or(name);
    if last.contains(':') { name.to_string() } else { format!("{name}:latest") }
}

/// A filesystem-safe directory name for an image reference.
fn safe_name(r: &ImageRef) -> String { r.canonical().replace(['/', ':'], "_") }

/// The image's default command from its OCI config blob (Entrypoint ++ Cmd), if present.
fn config_cmd(config: &Value) -> Option<Vec<String>> {
    let strs = |v: &Value| v.as_array().map(|a| a.iter().filter_map(|x| x.as_str().map(String::from)).collect::<Vec<_>>());
    let entry = strs(&config["config"]["Entrypoint"]).unwrap_or_default();
    let cmd = strs(&config["config"]["Cmd"]).unwrap_or_default();
    let argv: Vec<String> = entry.into_iter().chain(cmd).collect();
    (!argv.is_empty()).then_some(argv)
}
/// Fallback default command for an image whose config has no Cmd: prefer /bin/sh, else /bin/bash.
fn default_shell(rootfs: &std::path::Path) -> Vec<String> {
    for sh in ["/bin/sh", "/bin/bash"] {
        if rootfs.join(sh.trim_start_matches('/')).exists() { return vec![sh.to_string()]; }
    }
    vec!["/bin/sh".to_string()]
}

#[derive(Deserialize)]
struct CreateBody {
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
struct HostConfig {
    #[serde(rename = "Binds")] binds: Option<Vec<String>>,
    #[serde(rename = "Memory")] memory: Option<i64>,
    #[serde(rename = "PidsLimit")] pids_limit: Option<i64>,
    #[serde(rename = "PortBindings")] port_bindings: Option<HashMap<String, Vec<PortBinding>>>,
    #[serde(rename = "NetworkMode")] network_mode: Option<String>,
}
#[derive(Deserialize, Clone)]
struct PortBinding { #[serde(rename = "HostPort")] host_port: Option<String> }
fn publish_str(pb: &HashMap<String, Vec<PortBinding>>) -> String {
    let mut v = Vec::new();
    for (k, binds) in pb {
        let cport = k.split('/').next().unwrap_or("");
        for b in binds { if let Some(hp) = &b.host_port { if !hp.is_empty() && !cport.is_empty() { v.push(format!("{hp}:{cport}")); } } }
    }
    v.join(",")
}

#[derive(Deserialize)]
struct CreateQ { name: Option<String>, platform: Option<String> }
async fn containers_create(State(a): State<App>, Query(cq): Query<CreateQ>, Json(body): Json<CreateBody>) -> Response {
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

/// Resolve a container ref (full id, **id prefix** like the docker CLI sends, or short name) to its
/// full map key. Docker clients show/round-trip the 12-char short id, so prefix resolution is
/// required for `docker logs/inspect/rm <shortid>` to work.
fn resolve_cid(g: &Inner, id: &str) -> Option<String> {
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

async fn containers_start(State(a): State<App>, Path(id): Path<String>) -> Response {
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
async fn containers_stop(State(a): State<App>, Path(id): Path<String>) -> Response {
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
async fn containers_restart(a: State<App>, id: Path<String>) -> Response { containers_start(a, id).await }

/// POST /containers/:id/attach -- hijack the connection and stream the guest's IO. `docker run` (no -d)
/// and `docker run -it` use this: stdout/stderr come back framed (raw in TTY mode), and the client's
/// stdin (for -i) is fed to the guest. The hijacked stream closes when the guest exits.
/// Drive a hijacked docker stream against a Live: fan guest stdout/stderr to the client (docker
/// multiplexed frames, or raw bytes in TTY mode) and feed the client's stdin into the guest. Shared by
/// container attach and exec. `rx` is subscribed synchronously so no output is missed if the guest
/// starts producing before the upgrade completes.
fn spawn_hijack_io(on_upgrade: hyper::upgrade::OnUpgrade, live: Arc<Live>, tty: bool) {
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

const HIJACK_HEADERS: [(&str, &str); 3] =
    [("Content-Type", "application/vnd.docker.raw-stream"), ("Connection", "Upgrade"), ("Upgrade", "tcp")];
fn hijack_response() -> Response {
    let mut b = Response::builder().status(StatusCode::SWITCHING_PROTOCOLS);
    for (k, v) in HIJACK_HEADERS { b = b.header(k, v); }
    b.body(Body::empty()).unwrap()
}

async fn containers_attach(State(a): State<App>, Path(id): Path<String>, req: Request) -> Response {
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
struct ExecCreateBody {
    #[serde(rename = "Cmd")] cmd: Option<Vec<String>>,
    #[serde(rename = "Tty")] tty: Option<bool>,
}
/// POST /containers/:id/exec -- create an exec (record the command). Run it with /exec/:id/start.
async fn exec_create(State(a): State<App>, Path(id): Path<String>, Json(body): Json<ExecCreateBody>) -> Response {
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
async fn exec_start(State(a): State<App>, Path(id): Path<String>, req: Request) -> Response {
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
async fn exec_inspect(State(a): State<App>, Path(id): Path<String>) -> Response {
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

// ---- image management: tag / rmi / push -------------------------------------
#[derive(Deserialize)]
struct TagQ { repo: Option<String> }
/// POST /images/:name/tag -- alias an image under a new repo name (same rootfs).
async fn image_tag(State(a): State<App>, Path(name): Path<String>, Query(q): Query<TagQ>) -> Response {
    let mut g = a.inner.lock().await;
    let Some(src) = g.images.iter().find(|i| ref_name(&i.name) == ref_name(&name)).cloned() else {
        return (StatusCode::NOT_FOUND, Json(json!({"message": format!("No such image: {name}")}))).into_response();
    };
    let bare = ref_name(&q.repo.unwrap_or_default()).to_string();
    if bare.is_empty() { return (StatusCode::BAD_REQUEST, Json(json!({"message": "repo required"}))).into_response(); }
    if !g.images.iter().any(|i| i.name == bare) {
        g.images.push(Image { name: bare, ..src });
    }
    StatusCode::CREATED.into_response()
}
/// DELETE /images/:name -- `docker rmi`.
async fn image_delete(State(a): State<App>, Path(name): Path<String>) -> Response {
    let mut g = a.inner.lock().await;
    let bare = ref_name(&name).to_string();
    let before = g.images.len();
    g.images.retain(|i| ref_name(&i.name) != bare);
    if g.images.len() == before {
        return (StatusCode::NOT_FOUND, Json(json!({"message": format!("No such image: {name}")}))).into_response();
    }
    Json(json!([{ "Untagged": format!("{bare}:latest") }, { "Deleted": format!("sha256:{}", fake_id(&bare)) }])).into_response()
}
/// POST /images/:name/push -- dd has no registry yet (images are local), so this is a clean no-op that
/// reports success rather than erroring. TODO(OCI): real registry push.
/// POST /images/:name/push -- re-tar the local rootfs into a single-layer image and upload it to its
/// registry (`docker.io/...`, `ghcr.io/...`, `localhost:5000/...`) using the CLI's credentials.
async fn image_push(State(a): State<App>, Path(name): Path<String>, Query(q): Query<PushQ>, headers: axum::http::HeaderMap) -> Response {
    let img = a.inner.lock().await.images.iter().find(|i| ref_name(&i.name) == ref_name(&name)).cloned();
    let Some(img) = img else {
        return push_progress(&name, Err(format!("No such image: {name}"))).into_response();
    };
    let tag = q.tag.filter(|t| !t.is_empty()).unwrap_or_else(|| "latest".into());
    let iref = image_ref(&name, &tag);
    let arch = docker_arch(img.arch).to_string();
    let os = img.arch.os().to_string(); // "darwin" for mac images, else "linux"
    let creds = registry_auth(&headers);
    let work = std::path::PathBuf::from(format!("{}/.push-{}", a.images_dir, std::process::id()));
    let res = tokio::task::spawn_blocking(move || {
        Client::new(iref, creds).push(std::path::Path::new(&img.rootfs), &img.cmd, &arch, &os, &work)
    }).await.unwrap_or_else(|e| Err(format!("push task crashed: {e}")));
    push_progress(&name, res).into_response()
}

#[derive(Deserialize)]
struct PushQ { tag: Option<String> }
/// docker arch label for a guest target.
fn docker_arch(g: Guest) -> &'static str { if g.arch() == "x86_64" { "amd64" } else { "arm64" } }
/// A docker `--platform` value ("linux/amd64", "arm64", …) mapped to dd's arch label, if recognized.
fn platform_arch(platform: Option<&str>) -> Option<&'static str> {
    match platform?.rsplit('/').next().unwrap_or("") {
        "amd64" | "x86_64" => Some("amd64"),
        "arm64" | "aarch64" => Some("arm64"),
        _ => None,
    }
}
/// Preferred arch list when pulling for a given platform: the requested one, else native-arm64 first.
fn platform_archs(platform: Option<&str>) -> Vec<&'static str> {
    match platform_arch(platform) { Some(a) => vec![a], None => vec!["arm64", "amd64"] }
}

/// docker-style push progress (a stream of JSON status lines, or an error line).
fn push_progress(name: &str, result: Result<String, String>) -> Response {
    let body = match result {
        Ok(digest) => [
            json!({ "status": format!("The push refers to repository [{name}]") }).to_string(),
            json!({ "status": "Pushed" }).to_string(),
            json!({ "status": format!("latest: digest: {digest}") }).to_string(),
        ].join("\r\n") + "\r\n",
        Err(e) => json!({ "errorDetail": { "message": e.clone() }, "error": e }).to_string() + "\r\n",
    };
    (StatusCode::OK, [("Content-Type", "application/json")], body).into_response()
}

// ---- container control: pause / rename / top / stats ------------------------
/// POST /containers/:id/(un)pause -- dd runs a container as one process group with no freezer cgroup;
/// accept and no-op so the CLI verbs succeed.
async fn containers_pause(State(a): State<App>, Path(id): Path<String>) -> Response { freeze(a, id, true).await }
async fn containers_unpause(State(a): State<App>, Path(id): Path<String>) -> Response { freeze(a, id, false).await }
/// docker pause/unpause. macOS has no freezer cgroup, but SIGSTOP/SIGCONT on the container's JIT process
/// freezes it (and its threads) just the same -- single-process / threaded containers (the common case)
/// freeze fully; a guest that forked separate host processes pauses its main process (best-effort).
async fn freeze(a: App, id: String, pause: bool) -> Response {
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
struct RenameQ { name: Option<String> }
async fn containers_rename(State(a): State<App>, Path(id): Path<String>, Query(q): Query<RenameQ>) -> Response {
    let mut g = a.inner.lock().await;
    let Some(full) = resolve_cid(&g, &id) else { return no_such(&id) };
    if let Some(name) = q.name {
        if let Some(c) = g.containers.get_mut(&full) { c.name = name.trim_start_matches('/').to_string(); }
    }
    save_state(&g, &a.state_path);
    StatusCode::NO_CONTENT.into_response()
}
/// GET /containers/:id/top -- `docker top` (one synthetic process; dd doesn't expose a guest process tree).
async fn containers_top(State(a): State<App>, Path(id): Path<String>) -> Response {
    let g = a.inner.lock().await;
    let Some(full) = resolve_cid(&g, &id) else { return no_such(&id) };
    let cmd = g.containers.get(&full).map(|c| c.cmd.join(" ")).unwrap_or_default();
    Json(json!({ "Titles": ["UID", "PID", "PPID", "C", "STIME", "TTY", "TIME", "CMD"],
        "Processes": [["root", "1", "0", "0", "00:00", "?", "00:00:00", cmd]] })).into_response()
}
/// GET /containers/:id/stats -- one stats sample (dd has no cgroup accounting yet; zeros, valid shape).
async fn containers_stats(State(a): State<App>, Path(id): Path<String>) -> Response {
    let g = a.inner.lock().await;
    if resolve_cid(&g, &id).is_none() { return no_such(&id) }
    Json(json!({ "read": "1970-01-01T00:00:00Z", "name": format!("/{}", &id[..12.min(id.len())]),
        "cpu_stats": { "cpu_usage": { "total_usage": 0 }, "system_cpu_usage": 0, "online_cpus": 1 },
        "precpu_stats": { "cpu_usage": { "total_usage": 0 }, "system_cpu_usage": 0 },
        "memory_stats": { "usage": 0, "limit": 0 }, "pids_stats": { "current": 1 },
        "networks": {}, "blkio_stats": {} })).into_response()
}

#[derive(Deserialize)]
struct ResizeQ { h: Option<u16>, w: Option<u16> }
/// POST /containers/:id/resize and /exec/:id/resize -- set the PTY window size (TIOCSWINSZ) for a tty
/// container/exec. Always 200 so `docker run -t` never prints "failed to resize tty".
async fn resize(State(a): State<App>, Path(id): Path<String>, Query(q): Query<ResizeQ>) -> Response {
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
async fn containers_wait(State(a): State<App>, Path(id): Path<String>) -> Response {
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
async fn containers_logs(State(a): State<App>, Path(id): Path<String>) -> Response {
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

/// One Docker log frame: `[stream(1B), 0,0,0, len(4B big-endian)] + payload`.
fn log_frame(stream: u8, data: &[u8]) -> Vec<u8> {
    let mut out = Vec::with_capacity(8 + data.len());
    out.push(stream);
    out.extend_from_slice(&[0, 0, 0]);
    out.extend_from_slice(&(data.len() as u32).to_be_bytes());
    out.extend_from_slice(data);
    out
}
async fn containers_inspect(State(a): State<App>, Path(id): Path<String>) -> Response {
    let g = a.inner.lock().await;
    match resolve_cid(&g, &id).and_then(|f| g.containers.get(&f)) {
        Some(c) => Json(json!({"Id": c.id, "Image": c.image, "Created": fmt_rfc3339(c.created),
            "State": {"Status": c.status, "ExitCode": c.exit_code, "Running": c.status == "running" || c.status == "paused", "Paused": c.status == "paused"},
            "Config": {"Cmd": c.cmd, "Hostname": c.hostname},
            "HostConfig": {"Binds": c.binds, "Memory": c.memory, "PidsLimit": c.pids_limit}})).into_response(),
        None => no_such(&id) }
}
#[derive(Deserialize)]
struct PsQ { all: Option<String> }
async fn containers_json(State(a): State<App>, Query(q): Query<PsQ>) -> Json<Value> {
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
async fn containers_delete(State(a): State<App>, Path(id): Path<String>) -> Response {
    let mut g = a.inner.lock().await;
    let full = match resolve_cid(&g, &id) { Some(f) => f, None => return no_such(&id) };
    if g.containers.remove(&full).is_some() {
        // Drop the container from any network membership too.
        for n in g.networks.iter_mut() { n.containers.retain(|c| c != &full); }
        save_state(&g, &a.state_path);
        StatusCode::NO_CONTENT.into_response()
    } else { no_such(&id) }
}
fn no_such(id: &str) -> Response {
    (StatusCode::NOT_FOUND, Json(json!({"message": format!("No such container: {id}")}))).into_response()
}

// ===================== docker build — a minimal Dockerfile builder =====================
// Not BuildKit: we copy the base image's rootfs, run each RUN in the JIT (writes persist in the new
// rootfs), COPY from the build context, track ENV/WORKDIR/CMD/ENTRYPOINT, and register the result as an
// image. Reuses pull (base must be local), the JIT spawn, the archive path-mapper, and image registration.
#[derive(Deserialize)]
struct BuildQ { t: Option<String>, dockerfile: Option<String> }

/// Parse a Dockerfile into (INSTRUCTION, args) pairs, honoring `\` line-continuations and `#` comments.
fn parse_dockerfile(text: &str) -> Vec<(String, String)> {
    let (mut out, mut acc) = (Vec::new(), String::new());
    for line in text.lines() {
        let l = line.trim_end();
        let t = l.trim_start();
        if acc.is_empty() && (t.is_empty() || t.starts_with('#')) { continue; }
        if let Some(s) = l.strip_suffix('\\') { acc.push_str(s.trim_start()); acc.push(' '); continue; }
        acc.push_str(t);
        if let Some((inst, args)) = acc.trim().split_once(char::is_whitespace) {
            out.push((inst.to_uppercase(), args.trim().to_string()));
        }
        acc.clear();
    }
    out
}

/// A `CMD`/`ENTRYPOINT` value: JSON-array exec form `["a","b"]` or a shell string (wrapped in sh -c).
fn parse_exec_form(args: &str) -> Vec<String> {
    let a = args.trim();
    if a.starts_with('[') {
        if let Ok(Value::Array(v)) = serde_json::from_str::<Value>(a) {
            return v.into_iter().filter_map(|x| x.as_str().map(String::from)).collect();
        }
    }
    vec!["/bin/sh".into(), "-c".into(), a.to_string()]
}

fn build_stream(lines: Vec<String>) -> Response {
    (StatusCode::OK, [("Content-Type", "application/json")], lines.join("\n") + "\n").into_response()
}
fn build_err(mut lines: Vec<String>, msg: String) -> Response {
    lines.push(json!({"errorDetail": {"message": msg.clone()}, "error": msg}).to_string());
    build_stream(lines)
}

async fn images_build(State(a): State<App>, Query(q): Query<BuildQ>, body: axum::body::Bytes) -> Response {
    let raw_tag = q.t.clone().filter(|t| !t.is_empty()).unwrap_or_else(|| "built:latest".into());
    let name = ref_name(&raw_tag);
    let dfname = q.dockerfile.filter(|d| !d.is_empty()).unwrap_or_else(|| "Dockerfile".into());
    let mut log: Vec<String> = Vec::new();

    // unpack the build context (a tar in the request body)
    let ctx = std::path::PathBuf::from(format!("{}/.build-ctx-{}", a.images_dir, std::process::id()));
    let _ = std::fs::remove_dir_all(&ctx);
    if std::fs::create_dir_all(&ctx).is_err() { return build_err(log, "cannot create build dir".into()); }
    let ctar = ctx.join(".context.tar");
    let cleanup = |ctx: &std::path::Path| { let _ = std::fs::remove_dir_all(ctx); };
    if std::fs::write(&ctar, &body).is_err() { cleanup(&ctx); return build_err(log, "cannot write context".into()); }
    if !matches!(std::process::Command::new("tar").arg("xf").arg(&ctar).arg("-C").arg(&ctx).status(), Ok(s) if s.success()) {
        cleanup(&ctx); return build_err(log, "cannot unpack build context".into());
    }
    let dockerfile = match std::fs::read_to_string(ctx.join(&dfname)) {
        Ok(d) => d, Err(_) => { cleanup(&ctx); return build_err(log, format!("Cannot locate specified Dockerfile: {dfname}")); }
    };
    let steps = parse_dockerfile(&dockerfile);
    let total = steps.len();

    // the new image's rootfs dir under DD_IMAGES
    let safe: String = name.chars().map(|c| if c.is_alphanumeric() || "._-".contains(c) { c } else { '_' }).collect();
    let img_dir = std::path::PathBuf::from(format!("{}/{}", a.images_dir, safe));
    let _ = std::fs::remove_dir_all(&img_dir);
    let mut rootfs = img_dir.join("rootfs"); // the CURRENT stage's rootfs (reassigned at each FROM)
    let mut stages: Vec<std::path::PathBuf> = Vec::new();      // stage index -> its rootfs (multi-stage)
    let mut stage_names: HashMap<String, usize> = HashMap::new(); // name/index -> stage index

    // image config built up across the instructions (inherited from the base at FROM, then mutated)
    let (mut arch, mut cmd, mut entrypoint, mut workdir, mut env, mut from_done) =
        (Guest::LinuxAarch64, Vec::<String>::new(), Vec::<String>::new(), String::new(), Vec::<String>::new(), false);

    for (i, (inst, args)) in steps.iter().enumerate() {
        log.push(json!({"stream": format!("Step {}/{} : {} {}\n", i + 1, total, inst, args)}).to_string());
        match inst.as_str() {
            "FROM" => {
                let base = args.split_whitespace().next().unwrap_or("").to_string();
                let pick = |im: &Image| (im.rootfs.clone(), im.arch, im.cmd.clone(), im.entrypoint.clone(), im.env.clone(), im.workdir.clone());
                let mut found = { let g = a.inner.lock().await;
                    g.images.iter().find(|im| ref_name(&im.name) == ref_name(&base)).map(&pick) };
                if found.is_none() {
                    // not local -> auto-pull the base like real docker build (reuses the registry pull)
                    log.push(json!({"stream": format!("Unable to find image '{base}' locally; pulling\n")}).to_string());
                    let (n, t) = match base.rsplit_once(':') {
                        Some((n, t)) if !t.contains('/') => (n.to_string(), t.to_string()),
                        _ => (base.clone(), "latest".to_string()),
                    };
                    let (dir, archs) = (a.images_dir.clone(), platform_archs(None));
                    match tokio::task::spawn_blocking(move || pull_image(&dir, &n, &t, Credentials::default(), &archs)).await
                        .unwrap_or_else(|e| Err(format!("pull task crashed: {e}"))) {
                        Ok(img) => { found = Some(pick(&img)); a.inner.lock().await.images.push(img); }
                        Err(e) => { cleanup(&ctx); return build_err(log, format!("pull of base image '{base}' failed: {e}")); }
                    }
                }
                let Some((base_rootfs, base_arch, base_cmd, base_ep, base_env, base_wd)) = found else {
                    cleanup(&ctx); return build_err(log, format!("base image '{base}' unavailable")); };
                arch = base_arch; cmd = base_cmd; entrypoint = base_ep; env = base_env; workdir = base_wd; // inherit base config
                // start a new build stage (its own rootfs); `FROM <base> AS <name>` names it.
                let sidx = stages.len();
                rootfs = img_dir.join(format!("_s{sidx}")).join("rootfs");
                stages.push(rootfs.clone());
                stage_names.insert(sidx.to_string(), sidx);
                let words: Vec<&str> = args.split_whitespace().collect();
                if let Some(nm) = words.iter().position(|w| w.eq_ignore_ascii_case("AS")).and_then(|i| words.get(i + 1)) {
                    stage_names.insert(nm.to_string(), sidx);
                }
                std::fs::create_dir_all(rootfs.parent().unwrap_or(&img_dir)).ok();
                if !matches!(std::process::Command::new("cp").arg("-a").arg(&base_rootfs).arg(&rootfs).status(), Ok(s) if s.success()) {
                    cleanup(&ctx); return build_err(log, "failed to copy base image rootfs".into()); }
                from_done = true;
            }
            _ if !from_done => { cleanup(&ctx); return build_err(log, "no FROM before the first instruction".into()); }
            "RUN" => {
                let mut cfg = SpawnConfig::new(workdir.clone(), rootfs.to_string_lossy().into_owned());
                cfg.env = env.iter().filter_map(|e| e.split_once('=').map(|(k, v)| (k.to_string(), v.to_string()))).collect();
                cfg.argv = vec!["/bin/sh".into(), "-c".into(), args.clone()];
                let Some((prog, cargs)) = cfg.command(arch) else { cleanup(&ctx); return build_err(log, "JIT not available for this arch".into()); };
                match tokio::process::Command::new(prog).args(cargs).output().await {
                    Ok(o) => {
                        if !o.stdout.is_empty() { log.push(json!({"stream": String::from_utf8_lossy(&o.stdout)}).to_string()); }
                        if !o.stderr.is_empty() { log.push(json!({"stream": String::from_utf8_lossy(&o.stderr)}).to_string()); }
                        if !o.status.success() {
                            cleanup(&ctx); return build_err(log, format!("The command '/bin/sh -c {}' returned a non-zero code: {}", args, o.status.code().unwrap_or(-1))); }
                    }
                    Err(e) => { cleanup(&ctx); return build_err(log, format!("RUN failed to start: {e}")); }
                }
            }
            "COPY" | "ADD" => {
                let from_stage = args.split_whitespace().find_map(|p| p.strip_prefix("--from="));
                let parts: Vec<&str> = args.split_whitespace().filter(|p| !p.starts_with("--")).collect();
                if parts.len() < 2 { cleanup(&ctx); return build_err(log, format!("{inst} needs a source and destination")); }
                let dst = parts[parts.len() - 1];
                let dst_guest = if dst.starts_with('/') { dst.to_string() } else { format!("{}/{}", workdir.trim_end_matches('/'), dst) };
                let dst_host = archive_host_path(&rootfs.to_string_lossy(), &[], &dst_guest);
                let into_dir = dst.ends_with('/') || parts.len() > 2;
                if into_dir { std::fs::create_dir_all(&dst_host).ok(); } else if let Some(p) = dst_host.parent() { std::fs::create_dir_all(p).ok(); }
                // COPY --from=<stage>: source is a path inside that stage's rootfs; else the build context.
                let src_root = match from_stage {
                    Some(s) => match stage_names.get(s) { Some(&idx) => stages[idx].clone(),
                        None => { cleanup(&ctx); return build_err(log, format!("COPY --from: unknown stage '{s}'")); } },
                    None => ctx.clone(),
                };
                for src in &parts[..parts.len() - 1] {
                    let src_host = if from_stage.is_some() { archive_host_path(&src_root.to_string_lossy(), &[], src) } else { src_root.join(src) };
                    if !matches!(std::process::Command::new("cp").arg("-a").arg(&src_host).arg(&dst_host).status(), Ok(s) if s.success()) {
                        cleanup(&ctx); return build_err(log, format!("{inst} {src}: not found")); }
                }
            }
            "ENV" => {
                // `ENV K V` or `ENV K=V`; stored as "K=V"
                let kv = if let Some((k, v)) = args.split_once('=') {
                    format!("{}={}", k.trim(), v.split_whitespace().next().unwrap_or("").trim_matches('"'))
                } else if let Some((k, v)) = args.split_once(char::is_whitespace) {
                    format!("{}={}", k.trim(), v.trim().trim_matches('"'))
                } else { String::new() };
                if !kv.is_empty() { env.retain(|e| e.split_once('=').map(|(k, _)| k) != kv.split_once('=').map(|(k, _)| k)); env.push(kv); }
            }
            "WORKDIR" => {
                workdir = if args.starts_with('/') { args.clone() } else { format!("{}/{}", workdir.trim_end_matches('/'), args) };
                let wh = archive_host_path(&rootfs.to_string_lossy(), &[], &workdir);
                std::fs::create_dir_all(&wh).ok();
            }
            "CMD" => cmd = parse_exec_form(args),
            "ENTRYPOINT" => entrypoint = parse_exec_form(args),
            _ => {} // EXPOSE/LABEL/ARG/MAINTAINER/USER/VOLUME/HEALTHCHECK — no rootfs effect in this builder
        }
    }
    if !from_done { cleanup(&ctx); return build_err(log, "Dockerfile had no FROM".into()); }
    cleanup(&ctx);

    // the LAST stage is the final image: move its rootfs to <img>/rootfs, drop the intermediate stages.
    let final_rootfs = stages.last().cloned().unwrap_or_else(|| rootfs.clone());
    let image_rootfs = img_dir.join("rootfs");
    if final_rootfs != image_rootfs {
        let _ = std::fs::remove_dir_all(&image_rootfs);
        if std::fs::rename(&final_rootfs, &image_rootfs).is_err() {
            let _ = std::process::Command::new("cp").arg("-a").arg(&final_rootfs).arg(&image_rootfs).status();
        }
    }
    for s in &stages { if let Some(p) = s.parent() { if p != img_dir { let _ = std::fs::remove_dir_all(p); } } }
    let rootfs = image_rootfs; // the registered image's rootfs

    // register the built image (persist the full config so it survives a daemon restart)
    if cmd.is_empty() && entrypoint.is_empty() { cmd = default_shell(&rootfs); }
    std::fs::write(img_dir.join("dd-image.json"),
        json!({"name": name, "cmd": cmd, "entrypoint": entrypoint, "env": env, "workdir": workdir}).to_string()).ok();
    let id = format!("{:x}", md5_like(&safe));
    {
        let mut g = a.inner.lock().await;
        g.images.retain(|im| ref_name(&im.name) != name);
        g.images.push(Image { name: name.to_string(), rootfs: rootfs.to_string_lossy().into_owned(), arch, cmd, entrypoint, env, workdir });
    }
    log.push(json!({"stream": format!("Successfully built {}\n", &id[..12.min(id.len())])}).to_string());
    log.push(json!({"stream": format!("Successfully tagged {raw_tag}\n")}).to_string());
    log.push(json!({"aux": {"ID": format!("sha256:{id}")}}).to_string());
    build_stream(log)
}

/// A cheap, stable hex id for a built image (not a real digest — just a handle for the CLI).
fn md5_like(s: &str) -> u64 {
    let mut h: u64 = 0xcbf29ce484222325;
    for b in s.bytes() { h ^= b as u64; h = h.wrapping_mul(0x100000001b3); }
    h
}

// ===================== docker cp — the /archive tar endpoints =====================
// `docker cp` tars a path out of (GET) / into (PUT) the container filesystem; HEAD returns a path-stat the
// CLI uses to pick file-vs-dir semantics. We operate on the container's rootfs (the overlay upper) -- the
// common case; bind-volume paths aren't redirected yet.
#[derive(serde::Deserialize)]
struct ArchiveQ { path: String }

/// Map a container path to its host path. A path inside a bind volume maps to the host volume dir (so
/// `docker cp` to e.g. ddcli's mounted cwd hits the real files); otherwise it lands in the container
/// rootfs (the overlay upper). `..` is lexically clamped inside whichever base so it can't escape.
fn archive_host_path(rootfs: &str, binds: &[String], path: &str) -> std::path::PathBuf {
    // bind volumes first (host:container with an absolute host source), same precedence as the JIT jail
    for b in binds {
        if let Some((host, cont)) = b.split_once(':') {
            if host.starts_with('/') && (path == cont || path.strip_prefix(cont).is_some_and(|r| r.starts_with('/'))) {
                return clamp_join(host, &path[cont.len()..]);
            }
        }
    }
    clamp_join(rootfs, path)
}

/// Join `rel` onto `base`, dropping `.`/`..` so the result stays within `base`.
fn clamp_join(base: &str, rel: &str) -> std::path::PathBuf {
    let root = std::path::Path::new(base).to_path_buf();
    let mut out = root.clone();
    for part in rel.split('/') {
        match part {
            "" | "." => {}
            ".." => { if out != root { out.pop(); } }
            p => out.push(p),
        }
    }
    out
}

/// Go `os.FileMode` bits for docker's path-stat (the CLI keys off the dir/symlink flags).
fn go_filemode(md: &std::fs::Metadata) -> u32 {
    use std::os::unix::fs::PermissionsExt;
    let mut m = md.permissions().mode() & 0o7777;
    let ft = md.file_type();
    if ft.is_dir() { m |= 1 << 31; }
    if ft.is_symlink() { m |= 1 << 27; }
    m
}

/// The `X-Docker-Container-Path-Stat` header value: base64(JSON{name,size,mode,mtime,linkTarget}).
fn path_stat_b64(host: &std::path::Path) -> Option<String> {
    use std::os::unix::fs::MetadataExt;
    let md = std::fs::symlink_metadata(host).ok()?;
    let name = host.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default();
    let link = if md.file_type().is_symlink() {
        std::fs::read_link(host).map(|p| p.to_string_lossy().into_owned()).unwrap_or_default()
    } else { String::new() };
    let stat = json!({"name": name, "size": md.len(), "mode": go_filemode(&md),
        "mtime": fmt_rfc3339(md.mtime()), "linkTarget": link});
    Some(base64_std(stat.to_string().as_bytes()))
}

/// Standard base64 (no line breaks).
fn base64_std(data: &[u8]) -> String {
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

async fn archive_head(State(a): State<App>, Path(id): Path<String>, Query(q): Query<ArchiveQ>) -> Response {
    let g = a.inner.lock().await;
    let Some(c) = resolve_cid(&g, &id).and_then(|f| g.containers.get(&f)) else { return no_such(&id); };
    match path_stat_b64(&archive_host_path(&c.rootfs, &c.binds, &q.path)) {
        Some(stat) => (StatusCode::OK, [("X-Docker-Container-Path-Stat", stat)]).into_response(),
        None => (StatusCode::NOT_FOUND, Json(json!({"message": format!("Could not find the file {} in container {id}", q.path)}))).into_response(),
    }
}

async fn archive_get(State(a): State<App>, Path(id): Path<String>, Query(q): Query<ArchiveQ>) -> Response {
    let (rootfs, binds) = { let g = a.inner.lock().await;
        let Some(c) = resolve_cid(&g, &id).and_then(|f| g.containers.get(&f)) else { return no_such(&id); };
        (c.rootfs.clone(), c.binds.clone()) };
    let host = archive_host_path(&rootfs, &binds, &q.path);
    let Some(stat) = path_stat_b64(&host) else {
        return (StatusCode::NOT_FOUND, Json(json!({"message": format!("Could not find the file {} in container {id}", q.path)}))).into_response(); };
    let parent = host.parent().unwrap_or(std::path::Path::new("/")).to_path_buf();
    let base = host.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_else(|| ".".into());
    match std::process::Command::new("tar").arg("cf").arg("-").arg("-C").arg(&parent).arg(&base).output() {
        Ok(o) if o.status.success() => (StatusCode::OK,
            [("Content-Type", "application/x-tar".to_string()), ("X-Docker-Container-Path-Stat", stat)], o.stdout).into_response(),
        Ok(o) => (StatusCode::INTERNAL_SERVER_ERROR, Json(json!({"message": String::from_utf8_lossy(&o.stderr)}))).into_response(),
        Err(e) => (StatusCode::INTERNAL_SERVER_ERROR, Json(json!({"message": e.to_string()}))).into_response(),
    }
}

async fn archive_put(State(a): State<App>, Path(id): Path<String>, Query(q): Query<ArchiveQ>, body: axum::body::Bytes) -> Response {
    let (rootfs, binds) = { let g = a.inner.lock().await;
        let Some(c) = resolve_cid(&g, &id).and_then(|f| g.containers.get(&f)) else { return no_such(&id); };
        (c.rootfs.clone(), c.binds.clone()) };
    let host = archive_host_path(&rootfs, &binds, &q.path);
    if !host.is_dir() {
        return (StatusCode::BAD_REQUEST, Json(json!({"message": format!("extraction point {} is not a directory", q.path)}))).into_response(); }
    let tmp = std::env::temp_dir().join(format!("dd-cp-{}.tar", std::process::id()));
    if let Err(e) = std::fs::write(&tmp, &body) {
        return (StatusCode::INTERNAL_SERVER_ERROR, Json(json!({"message": e.to_string()}))).into_response(); }
    let out = std::process::Command::new("tar").arg("xf").arg(&tmp).arg("-C").arg(&host).output();
    let _ = std::fs::remove_file(&tmp);
    match out {
        Ok(o) if o.status.success() => (StatusCode::OK, Json(json!({}))).into_response(),
        Ok(o) => (StatusCode::INTERNAL_SERVER_ERROR, Json(json!({"message": String::from_utf8_lossy(&o.stderr)}))).into_response(),
        Err(e) => (StatusCode::INTERNAL_SERVER_ERROR, Json(json!({"message": e.to_string()}))).into_response(),
    }
}

/// Build the `Ports` array Docker clients expect from our "host:container,..." publish string.
fn ports_json(publish: &str) -> Vec<Value> {
    publish.split(',').filter(|s| !s.is_empty()).filter_map(|p| p.split_once(':')).filter_map(|(h, c)| {
        Some(json!({"PublicPort": h.parse::<u16>().ok()?, "PrivatePort": c.parse::<u16>().ok()?, "Type": "tcp", "IP": "0.0.0.0"}))
    }).collect()
}

// ---- volumes ---------------------------------------------------------------

fn vol_json(v: &Vol) -> Value {
    json!({"Name": v.name, "Driver": "local", "Mountpoint": v.mountpoint,
        "CreatedAt": v.created_at.to_string(), "Scope": "local", "Labels": {}, "Options": {}})
}
async fn volumes_list(State(a): State<App>) -> Json<Value> {
    let g = a.inner.lock().await;
    Json(json!({"Volumes": g.volumes.iter().map(vol_json).collect::<Vec<_>>(), "Warnings": []}))
}
#[derive(Deserialize)]
struct VolumeCreateBody { #[serde(rename = "Name")] name: Option<String> }
async fn volumes_create(State(a): State<App>, Json(body): Json<VolumeCreateBody>) -> Response {
    let name = body.name.filter(|n| !n.is_empty()).unwrap_or_else(|| format!("vol_{}", fake_id("v")[..12].to_string()));
    if !name.chars().all(|c| c.is_ascii_alphanumeric() || c == '_' || c == '-' || c == '.') {
        return (StatusCode::BAD_REQUEST, Json(json!({"message": "invalid volume name"}))).into_response();
    }
    let mountpoint = PathBuf::from(&a.volumes_dir).join(&name);
    let _ = std::fs::create_dir_all(&mountpoint);
    let mut g = a.inner.lock().await;
    let v = if let Some(existing) = g.volumes.iter().find(|v| v.name == name).cloned() {
        existing
    } else {
        let v = Vol { name: name.clone(), mountpoint: mountpoint.to_string_lossy().into_owned(), created_at: now_secs() };
        g.volumes.push(v.clone());
        save_state(&g, &a.state_path);
        v
    };
    (StatusCode::CREATED, Json(vol_json(&v))).into_response()
}
async fn volume_inspect(State(a): State<App>, Path(name): Path<String>) -> Response {
    match a.inner.lock().await.volumes.iter().find(|v| v.name == name) {
        Some(v) => Json(vol_json(v)).into_response(),
        None => (StatusCode::NOT_FOUND, Json(json!({"message": format!("no such volume: {name}")}))).into_response(),
    }
}
async fn volume_delete(State(a): State<App>, Path(name): Path<String>) -> Response {
    let mut g = a.inner.lock().await;
    let before = g.volumes.len();
    g.volumes.retain(|v| v.name != name);
    if g.volumes.len() != before {
        let _ = std::fs::remove_dir_all(PathBuf::from(&a.volumes_dir).join(&name));
        save_state(&g, &a.state_path);
        StatusCode::NO_CONTENT.into_response()
    } else {
        (StatusCode::NOT_FOUND, Json(json!({"message": format!("no such volume: {name}")}))).into_response()
    }
}

// ---- networks --------------------------------------------------------------

fn net_json(n: &Net) -> Value {
    let containers: HashMap<String, Value> = n.containers.iter().map(|c| (c.clone(), json!({"Name": &c[..12.min(c.len())]}))).collect();
    json!({"Id": n.id, "Name": n.name, "Driver": n.driver, "Scope": n.scope,
        "Containers": containers, "Created": fmt_rfc3339(n.created), "EnableIPv6": false, "Internal": false,
        "IPAM": {"Driver": "default", "Config": []}})
}
async fn networks_list(State(a): State<App>) -> Json<Value> {
    let g = a.inner.lock().await;
    Json(json!(g.networks.iter().map(net_json).collect::<Vec<_>>()))
}
#[derive(Deserialize)]
struct NetCreateBody { #[serde(rename = "Name")] name: Option<String>, #[serde(rename = "Driver")] driver: Option<String> }
async fn networks_create(State(a): State<App>, Json(body): Json<NetCreateBody>) -> Response {
    let name = body.name.filter(|n| !n.is_empty()).unwrap_or_else(|| format!("net_{}", &fake_id("n")[..8]));
    let mut g = a.inner.lock().await;
    if let Some(n) = g.networks.iter().find(|n| n.name == name) {
        return (StatusCode::CONFLICT, Json(json!({"message": format!("network {name} already exists"), "Id": n.id}))).into_response();
    }
    let n = Net { id: fake_id(&format!("net-{name}")), name, driver: body.driver.unwrap_or_else(|| "bridge".into()), scope: "local".into(), containers: vec![], created: now_secs() };
    let id = n.id.clone();
    g.networks.push(n);
    save_state(&g, &a.state_path);
    (StatusCode::CREATED, Json(json!({"Id": id, "Warning": ""}))).into_response()
}
async fn network_inspect(State(a): State<App>, Path(id): Path<String>) -> Response {
    match a.inner.lock().await.networks.iter().find(|n| net_matches(n, &id)) {
        Some(n) => Json(net_json(n)).into_response(),
        None => network_404(&id),
    }
}
async fn network_delete(State(a): State<App>, Path(id): Path<String>) -> Response {
    let mut g = a.inner.lock().await;
    if g.networks.iter().any(|n| net_matches(n, &id) && is_predefined(&n.name)) {
        return (StatusCode::FORBIDDEN, Json(json!({"message": "predefined network cannot be removed"}))).into_response();
    }
    let before = g.networks.len();
    g.networks.retain(|n| !net_matches(n, &id));
    if g.networks.len() != before { save_state(&g, &a.state_path); StatusCode::NO_CONTENT.into_response() } else { network_404(&id) }
}
#[derive(Deserialize)]
struct NetAttachBody { #[serde(rename = "Container")] container: Option<String> }
async fn network_connect(State(a): State<App>, Path(id): Path<String>, Json(b): Json<NetAttachBody>) -> Response {
    let cid = b.container.unwrap_or_default();
    let mut g = a.inner.lock().await;
    let r = match g.networks.iter_mut().find(|n| net_matches(n, &id)) {
        Some(n) => { if !n.containers.contains(&cid) { n.containers.push(cid); } StatusCode::OK.into_response() }
        None => return network_404(&id),
    };
    save_state(&g, &a.state_path);
    r
}
async fn network_disconnect(State(a): State<App>, Path(id): Path<String>, Json(b): Json<NetAttachBody>) -> Response {
    let cid = b.container.unwrap_or_default();
    let mut g = a.inner.lock().await;
    let r = match g.networks.iter_mut().find(|n| net_matches(n, &id)) {
        Some(n) => { n.containers.retain(|c| c != &cid); StatusCode::OK.into_response() }
        None => return network_404(&id),
    };
    save_state(&g, &a.state_path);
    r
}
fn net_matches(n: &Net, id: &str) -> bool {
    n.id == id || n.name == id || n.id.starts_with(id)
}
fn is_predefined(name: &str) -> bool {
    matches!(name, "bridge" | "host" | "none")
}
fn network_404(id: &str) -> Response {
    (StatusCode::NOT_FOUND, Json(json!({"message": format!("no such network: {id}")}))).into_response()
}
fn default_networks() -> Vec<Net> {
    ["bridge", "host", "none"].iter().map(|name| Net {
        id: fake_id(&format!("net-{name}")), name: name.to_string(),
        driver: if *name == "bridge" { "bridge".into() } else { name.to_string() }, created: 0,
        scope: "local".into(), containers: vec![],
    }).collect()
}

/// Translate the container into a typed [`SpawnConfig`] and run it in the matching guest's JIT.
/// Named-volume binds (`name:/path`, no leading `/`) are resolved against `volumes_dir`.
/// Build the (program, args) that launches this container in the matching guest's JIT. `None` if no JIT
/// was built for the image's arch.
fn spawn_cfg(c: &Container, volumes_dir: &str, vols: &[Vol]) -> Option<(String, Vec<String>)> {
    let guest = c.arch.unwrap_or(Guest::LinuxAarch64);
    let mut cfg = SpawnConfig::new(String::new(), c.rootfs.clone()); // work_dir is the HOST cwd; leave empty
    cfg.argv = c.cmd.clone();
    // `-w DIR` (WorkingDir): the guest's initial cwd, read as DD_CWD by the runtime at startup.
    if !c.working_dir.is_empty() { cfg.env.push(("DD_CWD".into(), c.working_dir.clone())); }
    // container env (image ENV + `docker run -e`) -> one DD_GUEST_ENV var so the JIT forwards EXACTLY
    // these to the guest, never the daemon/host environment.
    if !c.env.is_empty() { cfg.env.push(("DD_GUEST_ENV".into(), c.env.join("\n"))); }
    cfg.hostname = (!c.hostname.is_empty()).then(|| c.hostname.clone());
    cfg.mem_max = c.memory.max(0) as u64;
    cfg.pids_max = c.pids_limit.max(0) as u32;
    // `--network host` shares the host network (no per-container netns); otherwise isolate in one.
    cfg.netns = (c.network_mode != "host").then(|| c.id[..c.id.len().min(40)].to_string());
    // `--network none`: no external egress -- the JIT refuses non-loopback connects (DD_NET_ISOLATE).
    if c.network_mode == "none" { cfg.env.push(("DD_NET_ISOLATE".into(), "1".into())); }
    cfg.volumes = c.binds.iter().filter_map(|b| b.split_once(':').map(|(host, dst)| {
        // A bind whose source isn't an absolute path is a named volume.
        let host = if host.starts_with('/') {
            host.to_string()
        } else if let Some(v) = vols.iter().find(|v| v.name == host) {
            v.mountpoint.clone()
        } else {
            PathBuf::from(volumes_dir).join(host).to_string_lossy().into_owned()
        };
        Volume { container: dst.into(), host }
    })).collect();
    cfg.publish = c.publish.split(',').filter(|s| !s.is_empty()).filter_map(|p| p.split_once(':'))
        .filter_map(|(h, cc)| Some(PortMap { host: h.parse().ok()?, container: cc.parse().ok()? })).collect();
    // macOS containers (darwinjail): the userland (nix arm64 tools) is on PATH at /profile/bin, and the
    // host filesystem is the read-only lower so native binaries find their /nix deps; writes land in the
    // rootfs + volumes. The entry argv[0] is run by the mac shell, so it must be a real host path -- we
    // exec it through the profile's bash, which resolves the command via the in-jail PATH (so a bare
    // `bash`/`uname`/… is found at /profile/bin and execve()'d into the jail).
    if guest.os() == "darwin" {
        cfg.lowers = vec!["/".into()];
        cfg.env.push(("PATH".into(), "/profile/bin".into()));
        // `-c` (not `-lc`): a login shell would source the host's /etc/profile via the lower and exec
        // arm64e system tools (which the arm64 jail can't inject into); the container has its own env.
        let wrapper = format!("{}/profile/bin/bash", c.rootfs);
        let mut wrapped = vec![wrapper, "-c".into(), "exec \"$@\"".into(), "dd-mac".into()];
        wrapped.extend(std::mem::take(&mut cfg.argv));
        cfg.argv = wrapped;
    }
    cfg.command(guest)
}

/// Spawn the container's guest process live (piped stdio) and wire its IO into `live`: stdout/stderr fan
/// out to attached clients + the log buffers; on exit, the container's status/exit-code are finalized.
/// Idempotent per container (start is a no-op if already running). Returns false if no JIT for the arch.
async fn spawn_live(app: &App, c: &Container, vols: &[Vol], live: Arc<Live>) -> bool {
    use std::sync::atomic::Ordering;
    if live.started.swap(true, Ordering::SeqCst) {
        return true; // already started
    }
    let Some((prog, args)) = spawn_cfg(c, &app.volumes_dir, vols) else { return false };
    let mut cmd = tokio::process::Command::new(prog);
    cmd.args(args);

    // pump one piped stream: broadcast each chunk to attached clients + accumulate for `docker logs`.
    async fn pump(mut r: impl AsyncReadExt + Unpin, kind: u8, live: Arc<Live>) {
        let mut buf = [0u8; 8192];
        loop {
            match r.read(&mut buf).await {
                Ok(0) | Err(_) => break,
                Ok(n) => {
                    let chunk = buf[..n].to_vec();
                    let _ = live.out.send((kind, chunk.clone()));
                    let b = if kind == 1 { &live.stdout_buf } else { &live.stderr_buf };
                    b.lock().await.extend_from_slice(&chunk);
                }
            }
        }
    }

    // tty=true gives the guest a real PTY -- an interactive shell sees a terminal (prompt, line editing),
    // and stdout/stderr merge into one raw stream. Otherwise stdio is piped (the multiplexed-frame path).
    let (mut child, io_handles): (tokio::process::Child, Vec<tokio::task::JoinHandle<()>>) = if c.tty {
        let (master, slave) = match open_pty() {
            Ok(p) => p,
            Err(e) => return live_fail(app, &c.id, &live, format!("openpty: {e}")).await,
        };
        let slave_fd = slave.as_raw_fd();
        cmd.stdin(Stdio::null()).stdout(Stdio::null()).stderr(Stdio::null());
        // SAFETY: in the forked child, login_tty makes the slave the controlling terminal + stdin/out/err.
        unsafe {
            cmd.pre_exec(move || {
                if libc::login_tty(slave_fd) != 0 { return Err(std::io::Error::last_os_error()); }
                Ok(())
            });
        }
        let child = match cmd.spawn() {
            Ok(ch) => ch,
            Err(e) => return live_fail(app, &c.id, &live, format!("jit exec failed: {e}")).await,
        };
        drop(slave); // the child dup'd it via login_tty; close the parent's copy
        set_nonblocking(master.as_raw_fd());
        *live.pty_master.lock().unwrap() = Some(master.as_raw_fd());
        let afd = match AsyncFd::new(master) {
            Ok(a) => Arc::new(a),
            Err(e) => return live_fail(app, &c.id, &live, format!("asyncfd: {e}")).await,
        };
        // client stdin -> PTY master
        if let Some(mut rx) = live.stdin_rx.lock().await.take() {
            let w = afd.clone();
            tokio::spawn(async move {
                while let Some(chunk) = rx.recv().await {
                    if chunk.is_empty() { break; }
                    let mut off = 0;
                    while off < chunk.len() {
                        let Ok(mut g) = w.writable().await else { return };
                        match g.try_io(|i| pty_write(i.as_raw_fd(), &chunk[off..])) {
                            Ok(Ok(n)) => off += n,
                            Ok(Err(_)) => return,
                            Err(_would_block) => continue,
                        }
                    }
                }
            });
        }
        // PTY master -> broadcast (kind 1) + log buffer
        let r = afd.clone();
        let lr = live.clone();
        let reader = tokio::spawn(async move {
            loop {
                let Ok(mut g) = r.readable().await else { break };
                let mut buf = [0u8; 8192];
                match g.try_io(|i| pty_read(i.as_raw_fd(), &mut buf)) {
                    Ok(Ok(0)) | Ok(Err(_)) => break, // EOF / EIO when the guest exits
                    Ok(Ok(n)) => {
                        let chunk = buf[..n].to_vec();
                        let _ = lr.out.send((1, chunk.clone()));
                        lr.stdout_buf.lock().await.extend_from_slice(&chunk);
                    }
                    Err(_would_block) => continue,
                }
            }
        });
        (child, vec![reader])
    } else {
        cmd.stdin(Stdio::piped()).stdout(Stdio::piped()).stderr(Stdio::piped());
        let mut child = match cmd.spawn() {
            Ok(ch) => ch,
            Err(e) => return live_fail(app, &c.id, &live, format!("jit exec failed: {e}")).await,
        };
        let stdout = child.stdout.take().unwrap();
        let stderr = child.stderr.take().unwrap();
        // Feed the guest's stdin from the channel attach writes to (buffered until now). An empty Vec is
        // the stdin-EOF sentinel: close the guest's stdin so a shell reading to EOF can finish.
        if let Some(mut child_in) = child.stdin.take() {
            if let Some(mut rx) = live.stdin_rx.lock().await.take() {
                tokio::spawn(async move {
                    while let Some(chunk) = rx.recv().await {
                        if chunk.is_empty() { break; }
                        if child_in.write_all(&chunk).await.is_err() { break; }
                        let _ = child_in.flush().await;
                    }
                    drop(child_in); // EOF to the guest
                });
            }
        }
        let lo = live.clone();
        let h_out = tokio::spawn(async move { pump(stdout, 1, lo).await; });
        let le = live.clone();
        let h_err = tokio::spawn(async move { pump(stderr, 2, le).await; });
        (child, vec![h_out, h_err])
    };

    *live.pid.lock().unwrap() = child.id(); // remember the pid so pause can SIGSTOP/SIGCONT it
    let app = app.clone();
    let cid = c.id.clone();
    let dbg = std::env::var("DD_DEBUG").is_ok();
    tokio::spawn(async move {
        let code = child.wait().await.ok().and_then(|s| s.code()).unwrap_or(-1) as i64;
        if dbg { eprintln!("[live] {} exited code={code}", &cid[..12]); }
        for h in io_handles { let _ = h.await; }
        *live.pty_master.lock().unwrap() = None;
        {
            let mut g = app.inner.lock().await;
            if let Some(cc) = g.containers.get_mut(&cid) {
                cc.status = "exited".into();
                cc.exit_code = code;
                cc.stdout = std::mem::take(&mut *live.stdout_buf.lock().await);
                cc.stderr = std::mem::take(&mut *live.stderr_buf.lock().await);
            }
            save_state(&g, &app.state_path);
        }
        let _ = live.exit.send(Some(code));
    });
    true
}

/// Record the failure on a Live and finalize the container as exit 127. Returns false (spawn failed).
async fn live_fail(app: &App, cid: &str, live: &Arc<Live>, msg: String) -> bool {
    let _ = live.out.send((2, format!("{msg}\n").into_bytes()));
    *live.stderr_buf.lock().await = format!("{msg}\n").into_bytes();
    let _ = live.exit.send(Some(127));
    if let Some(cc) = app.inner.lock().await.containers.get_mut(cid) { cc.status = "exited".into(); cc.exit_code = 127; }
    false
}

/// Allocate a pseudo-terminal; returns (master, slave) owned fds.
fn open_pty() -> std::io::Result<(OwnedFd, OwnedFd)> {
    let (mut m, mut s): (RawFd, RawFd) = (-1, -1);
    // termios/winsize are *mut on macOS, *const on linux; null_mut() coerces to both.
    let r = unsafe { libc::openpty(&mut m, &mut s, std::ptr::null_mut(), std::ptr::null_mut(), std::ptr::null_mut()) };
    if r != 0 { return Err(std::io::Error::last_os_error()); }
    Ok(unsafe { (OwnedFd::from_raw_fd(m), OwnedFd::from_raw_fd(s)) })
}
fn set_nonblocking(fd: RawFd) {
    unsafe {
        let fl = libc::fcntl(fd, libc::F_GETFL);
        libc::fcntl(fd, libc::F_SETFL, fl | libc::O_NONBLOCK);
    }
}
fn pty_read(fd: RawFd, buf: &mut [u8]) -> std::io::Result<usize> {
    let n = unsafe { libc::read(fd, buf.as_mut_ptr().cast(), buf.len()) };
    if n < 0 { Err(std::io::Error::last_os_error()) } else { Ok(n as usize) }
}
fn pty_write(fd: RawFd, buf: &[u8]) -> std::io::Result<usize> {
    let n = unsafe { libc::write(fd, buf.as_ptr().cast(), buf.len()) };
    if n < 0 { Err(std::io::Error::last_os_error()) } else { Ok(n as usize) }
}

// ---- persistence -----------------------------------------------------------

/// `~/.dd` (or `./.dd` if `$HOME` is unset) — the default state/volumes root.
fn dd_home() -> PathBuf {
    std::env::var_os("HOME").map(PathBuf::from).unwrap_or_else(|| PathBuf::from(".")).join(".dd")
}

fn now_secs() -> i64 {
    SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_secs() as i64).unwrap_or(0)
}

/// Format a unix timestamp as an RFC3339 UTC string (Docker's inspect `Created` is a string).
/// Pure integer civil-date math (Howard Hinnant's algorithm) — no chrono dependency.
fn fmt_rfc3339(secs: i64) -> String {
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
fn save_state(inner: &Inner, path: &str) {
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
fn load_state(inner: &mut Inner, path: &str) {
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
fn discover_images(images_dir: &str) -> Vec<Image> {
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
                // os:darwin marks a native-macOS (darwinjail) image; otherwise detect from the rootfs.
                let arch = if m["os"].as_str() == Some("darwin") { Guest::DarwinAarch64 }
                           else { detect_arch(&rootfs).unwrap_or(Guest::LinuxAarch64) };
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
        let workdir = meta.as_ref().and_then(|m| m["workdir"].as_str()).unwrap_or("").to_string();
        out.push(Image { name, rootfs: rootfs.to_string_lossy().into_owned(), arch, cmd, env: arr("env"), entrypoint: arr("entrypoint"), workdir });
    }
    out
}

/// Probe a likely executable in the rootfs and pick the guest target from its binary magic:
/// ELF -> linux (e_machine = aarch64/x86_64), Mach-O 64 -> darwin (cputype = arm64).
fn detect_arch(rootfs: &std::path::Path) -> Option<Guest> {
    for probe in ["bin/busybox", "bin/sh", "bin/true", "usr/bin/coreutils", "usr/lib/dyld"] {
        let p = rootfs.join(probe);
        if let Ok(b) = std::fs::read(&p) {
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
        }
    }
    None
}

fn fake_id(seed: &str) -> String {
    let mut h: u64 = 1469598103934665603;
    for b in seed.bytes() { h ^= b as u64; h = h.wrapping_mul(1099511628211); }
    format!("{h:016x}{h:016x}{h:016x}{h:08x}")
}

/// On-disk size of an image's rootfs, cached per rootfs path (computed once; rootfs rarely changes).
/// The host-fs `macos` image is skipped (walking `/` would be catastrophic).
fn image_size(rootfs: &str, name: &str) -> i64 {
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
fn dir_size(p: &std::path::Path) -> i64 {
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
fn ref_name(s: &str) -> &str {
    let last = s.rsplit('/').next().unwrap_or(s);
    last.split('@').next().unwrap_or(last).split(':').next().unwrap_or(last)
}

/// A unique container id. Seeded from a never-reset process counter + nanosecond clock, so it stays
/// unique even as containers are created and deleted (a count-based seed would collide after a rm).
fn new_id(image: &str) -> String {
    use std::sync::atomic::{AtomicU64, Ordering};
    static SEQ: AtomicU64 = AtomicU64::new(0);
    let seq = SEQ.fetch_add(1, Ordering::Relaxed);
    let nanos = SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_nanos() as u64).unwrap_or(0);
    fake_id(&format!("{image}-{seq}-{nanos}"))
}

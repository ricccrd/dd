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
    extract::{Path, Query, State},
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
use tokio::sync::Mutex;

const API_VERSION: &str = "1.43";

#[derive(Clone)]
struct Image {
    name: String,
    rootfs: String,
    arch: Guest,
    cmd: Vec<String>,
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
    // Re-derived from the image at load; never serialized.
    #[serde(skip)]
    arch: Option<Guest>,
    // Captured output is not persisted (would bloat the state file).
    #[serde(skip)]
    stdout: Vec<u8>,
    #[serde(skip)]
    stderr: Vec<u8>,
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

#[derive(Default)]
struct Inner {
    containers: HashMap<String, Container>,
    images: Vec<Image>,
    volumes: Vec<Vol>,
    networks: Vec<Net>,
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
    let app = App { inner: Arc::new(Mutex::new(inner)), state_path, volumes_dir };

    let router = Router::new()
        .route("/_ping", get(|| async { "OK" }))
        .route("/version", get(version)).route("/info", get(info))
        .route("/images/json", get(images_json)).route("/images/create", post(images_create))
        .route("/containers/json", get(containers_json))
        .route("/containers/create", post(containers_create))
        .route("/containers/:id/start", post(containers_start))
        .route("/containers/:id/stop", post(containers_stop))
        .route("/containers/:id/kill", post(containers_stop))
        .route("/containers/:id/restart", post(containers_restart))
        .route("/containers/:id/wait", post(containers_wait))
        .route("/containers/:id/logs", get(containers_logs))
        .route("/containers/:id/json", get(containers_inspect))
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
                tower::ServiceExt::oneshot(svc.clone(), req)
            });
            let _ = hyper_util::server::conn::auto::Builder::new(hyper_util::rt::TokioExecutor::new())
                .serve_connection(io, hsvc).await;
        });
    }
}

fn strip_api_version<B>(req: &mut hyper::Request<B>) {
    let pq = req.uri().path_and_query().map(|p| p.as_str().to_string()).unwrap_or_default();
    if let Some(rest) = pq.strip_prefix("/v1.") {
        if let Some(slash) = rest.find('/') {
            if let Ok(uri) = rest[slash..].parse::<Uri>() { *req.uri_mut() = uri; }
        }
    }
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
        "Id": format!("sha256:{}", fake_id(&i.name)), "RepoTags": [format!("{}:latest", i.name)],
        "Architecture": i.arch.target(), "Created": 0, "Size": 0})).collect();
    Json(json!(imgs))
}
#[derive(Deserialize)]
struct ImageCreateQ { #[serde(rename = "fromImage")] from_image: Option<String> }
async fn images_create(Query(q): Query<ImageCreateQ>) -> Response {
    // TODO(OCI pull): registry pull + unpack -> overlay lower dirs. Today: local rootfs only.
    let name = q.from_image.unwrap_or_default();
    (StatusCode::OK, Json(json!({"status": format!("Using local image {name}")}))).into_response()
}

#[derive(Deserialize)]
struct CreateBody {
    #[serde(rename = "Image")] image: Option<String>,
    #[serde(rename = "Cmd")] cmd: Option<Vec<String>>,
    #[serde(rename = "Hostname")] hostname: Option<String>,
    #[serde(rename = "HostConfig")] host_config: Option<HostConfig>,
}
#[derive(Deserialize)]
struct HostConfig {
    #[serde(rename = "Binds")] binds: Option<Vec<String>>,
    #[serde(rename = "Memory")] memory: Option<i64>,
    #[serde(rename = "PidsLimit")] pids_limit: Option<i64>,
    #[serde(rename = "PortBindings")] port_bindings: Option<HashMap<String, Vec<PortBinding>>>,
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

async fn containers_create(State(a): State<App>, Json(body): Json<CreateBody>) -> Response {
    let image = body.image.unwrap_or_default();
    let mut g = a.inner.lock().await;
    let img = match g.images.iter().find(|i| i.name == image || format!("{}:latest", i.name) == image).cloned() {
        Some(i) => i,
        None => return (StatusCode::NOT_FOUND, Json(json!({"message": format!("No such image: {image}")}))).into_response(),
    };
    let cmd = body.cmd.filter(|c| !c.is_empty()).unwrap_or_else(|| img.cmd.clone());
    let id = new_id(&image);
    let hc = body.host_config;
    let c = Container {
        id: id.clone(), image, rootfs: img.rootfs, cmd, arch: Some(img.arch),
        binds: hc.as_ref().and_then(|h| h.binds.clone()).unwrap_or_default(),
        hostname: body.hostname.unwrap_or_default(),
        memory: hc.as_ref().and_then(|h| h.memory).unwrap_or(0),
        pids_limit: hc.as_ref().and_then(|h| h.pids_limit).unwrap_or(0),
        publish: hc.as_ref().and_then(|h| h.port_bindings.as_ref()).map(publish_str).unwrap_or_default(),
        created: now_secs(),
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
    // Fall back to the short-id "name" we expose in containers_json.
    g.containers.keys().find(|k| k.get(..12).map(|p| p == id).unwrap_or(false)).cloned()
}

async fn containers_start(State(a): State<App>, Path(id): Path<String>) -> Response {
    let (c, vols) = {
        let g = a.inner.lock().await;
        let full = match resolve_cid(&g, &id) { Some(f) => f, None => return no_such(&id) };
        match g.containers.get(&full).cloned() { Some(c) => (c, g.volumes.clone()), None => return no_such(&id) }
    };
    let (out, err, code) = run_in_jit(&c, &a.volumes_dir, &vols).await;
    let mut g = a.inner.lock().await;
    if let Some(c) = g.containers.get_mut(&c.id) {
        c.stdout = out; c.stderr = err; c.exit_code = code; c.status = "exited".into();
    }
    save_state(&g, &a.state_path);
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

async fn containers_wait(State(a): State<App>, Path(id): Path<String>) -> Response {
    let g = a.inner.lock().await;
    match resolve_cid(&g, &id).and_then(|f| g.containers.get(&f)) {
        Some(c) => Json(json!({"StatusCode": c.exit_code})).into_response(), None => no_such(&id) }
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
            "State": {"Status": c.status, "ExitCode": c.exit_code, "Running": c.status == "running"},
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
        "Names": [format!("/{}", &c.id[..12.min(c.id.len())])]})).collect();
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
async fn run_in_jit(c: &Container, volumes_dir: &str, vols: &[Vol]) -> (Vec<u8>, Vec<u8>, i64) {
    let guest = c.arch.unwrap_or(Guest::LinuxAarch64);
    let mut cfg = SpawnConfig::new(String::new(), c.rootfs.clone()); // absolute paths -> no work_dir cd
    cfg.argv = c.cmd.clone();
    cfg.hostname = (!c.hostname.is_empty()).then(|| c.hostname.clone());
    cfg.mem_max = c.memory.max(0) as u64;
    cfg.pids_max = c.pids_limit.max(0) as u32;
    cfg.netns = Some(c.id[..c.id.len().min(40)].to_string());
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

    let (prog, args) = match cfg.command(guest) {
        Some(x) => x,
        None => return (vec![], format!("no JIT built for guest arch {}\n", guest.target()).into_bytes(), 127),
    };
    match tokio::process::Command::new(prog).args(args).output().await {
        Ok(o) => (o.stdout, o.stderr, o.status.code().unwrap_or(-1) as i64),
        Err(e) => (vec![], format!("jit exec failed: {e}\n").into_bytes(), 127),
    }
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
        let raw = e.path().file_name().and_then(|s| s.to_str()).unwrap_or("img").to_string();
        let name = raw.trim_end_matches("-bundle").split("__").next().unwrap_or("img").rsplit('_').next().unwrap_or("img").to_string();
        let arch = detect_arch(&rootfs).unwrap_or(Guest::LinuxAarch64);
        out.push(Image { name, rootfs: rootfs.to_string_lossy().into_owned(), arch, cmd: vec!["/bin/sh".into()] });
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

/// A unique container id. Seeded from a never-reset process counter + nanosecond clock, so it stays
/// unique even as containers are created and deleted (a count-based seed would collide after a rm).
fn new_id(image: &str) -> String {
    use std::sync::atomic::{AtomicU64, Ordering};
    static SEQ: AtomicU64 = AtomicU64::new(0);
    let seq = SEQ.fetch_add(1, Ordering::Relaxed);
    let nanos = SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_nanos() as u64).unwrap_or(0);
    fake_id(&format!("{image}-{seq}-{nanos}"))
}

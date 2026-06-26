//! dd-daemon — a Docker-Engine-API daemon backed by the **dd** VM-less JIT runtime.
//!
//! The real `docker` CLI talks to this over a Unix socket; container *execution* is delegated to the
//! JIT binaries built by the `ddjit` crate (one per guest architecture). The daemon detects each
//! image's architecture from its ELF and picks the matching JIT, then launches it via the typed
//! [`ddjit::SpawnConfig`] contract — no VM.
//!
//!   cargo run --release -p dd-daemon            # build.rs builds the JITs first
//!   DOCKER_HOST=unix://$PWD/dd.sock docker run -p 8080:80 -m 256m alpine echo hi
//!
//! Env: DD_IMAGES (dir of <name>/rootfs image dirs; default "./images"), DDOCKERD_SOCK (listen socket).

use axum::{
    extract::{Path, State},
    http::{StatusCode, Uri},
    response::{IntoResponse, Response},
    routing::{delete, get, post},
    Json, Router,
};
use ddjit::{Guest, PortMap, SpawnConfig, Volume};
use serde::Deserialize;
use serde_json::{json, Value};
use std::collections::HashMap;
use std::sync::Arc;
use tokio::sync::Mutex;

const API_VERSION: &str = "1.43";

#[derive(Clone)]
struct Image { name: String, rootfs: String, arch: Guest, cmd: Vec<String> }

#[derive(Clone, Default)]
struct Container {
    id: String, image: String, rootfs: String, cmd: Vec<String>, binds: Vec<String>,
    hostname: String, memory: i64, pids_limit: i64, publish: String,
    arch: Option<Guest>, status: String, exit_code: i64, stdout: Vec<u8>, stderr: Vec<u8>,
}

#[derive(Default)]
struct Inner { containers: HashMap<String, Container>, images: Vec<Image> }

#[derive(Clone)]
struct App { inner: Arc<Mutex<Inner>>, images_dir: String }

#[tokio::main]
async fn main() {
    let images_dir = std::env::var("DD_IMAGES").unwrap_or_else(|_| "./images".into());
    let sock = std::env::var("DDOCKERD_SOCK").unwrap_or_else(|_| "./dd.sock".into());
    let _ = std::fs::remove_file(&sock);

    let mut inner = Inner::default();
    inner.images = discover_images(&images_dir);
    eprintln!("[dd-daemon] images={} -> {} image(s): {}", images_dir, inner.images.len(),
        inner.images.iter().map(|i| format!("{}({})", i.name, i.arch.as_str())).collect::<Vec<_>>().join(", "));
    for g in [Guest::Aarch64, Guest::X86_64] {
        eprintln!("[dd-daemon] JIT {}: {}", g.as_str(), if ddjit::available(g) { "ready" } else { "NOT BUILT" });
    }
    let app = App { inner: Arc::new(Mutex::new(inner)), images_dir };

    let router = Router::new()
        .route("/_ping", get(|| async { "OK" }))
        .route("/version", get(version)).route("/info", get(info))
        .route("/images/json", get(images_json)).route("/images/create", post(images_create))
        .route("/containers/json", get(containers_json))
        .route("/containers/create", post(containers_create))
        .route("/containers/:id/start", post(containers_start))
        .route("/containers/:id/wait", post(containers_wait))
        .route("/containers/:id/logs", get(containers_logs))
        .route("/containers/:id/json", get(containers_inspect))
        .route("/containers/:id", delete(containers_delete))
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
        "Driver": "jit-overlay", "OperatingSystem": "dd (VM-less JIT on macOS)",
        "OSType": "linux", "Architecture": "aarch64", "NCPU": 1, "ServerVersion": "0.1.0-dd"}))
}
async fn images_json(State(a): State<App>) -> Json<Value> {
    let imgs: Vec<Value> = a.inner.lock().await.images.iter().map(|i| json!({
        "Id": format!("sha256:{}", fake_id(&i.name)), "RepoTags": [format!("{}:latest", i.name)],
        "Architecture": i.arch.as_str(), "Created": 0, "Size": 0})).collect();
    Json(json!(imgs))
}
#[derive(Deserialize)]
struct ImageCreateQ { #[serde(rename = "fromImage")] from_image: Option<String> }
async fn images_create(axum::extract::Query(q): axum::extract::Query<ImageCreateQ>) -> Response {
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
    let id = fake_id(&format!("{image}-{}", g.containers.len()));
    let hc = body.host_config;
    let c = Container {
        id: id.clone(), image, rootfs: img.rootfs, cmd, arch: Some(img.arch),
        binds: hc.as_ref().and_then(|h| h.binds.clone()).unwrap_or_default(),
        hostname: body.hostname.unwrap_or_default(),
        memory: hc.as_ref().and_then(|h| h.memory).unwrap_or(0),
        pids_limit: hc.as_ref().and_then(|h| h.pids_limit).unwrap_or(0),
        publish: hc.as_ref().and_then(|h| h.port_bindings.as_ref()).map(publish_str).unwrap_or_default(),
        status: "created".into(), ..Default::default()
    };
    g.containers.insert(id.clone(), c);
    (StatusCode::CREATED, Json(json!({"Id": id, "Warnings": []}))).into_response()
}

async fn containers_start(State(a): State<App>, Path(id): Path<String>) -> Response {
    let c = match a.inner.lock().await.containers.get(&id).cloned() { Some(c) => c, None => return no_such(&id) };
    let (out, err, code) = run_in_jit(&c).await;
    if let Some(c) = a.inner.lock().await.containers.get_mut(&id) {
        c.stdout = out; c.stderr = err; c.exit_code = code; c.status = "exited".into();
    }
    StatusCode::NO_CONTENT.into_response()
}
async fn containers_wait(State(a): State<App>, Path(id): Path<String>) -> Response {
    match a.inner.lock().await.containers.get(&id) {
        Some(c) => Json(json!({"StatusCode": c.exit_code})).into_response(), None => no_such(&id) }
}
async fn containers_logs(State(a): State<App>, Path(id): Path<String>) -> Response {
    match a.inner.lock().await.containers.get(&id) {
        Some(c) => { let mut b = c.stdout.clone(); b.extend_from_slice(&c.stderr); b.into_response() } None => no_such(&id) }
}
async fn containers_inspect(State(a): State<App>, Path(id): Path<String>) -> Response {
    match a.inner.lock().await.containers.get(&id) {
        Some(c) => Json(json!({"Id": c.id, "Image": c.image,
            "State": {"Status": c.status, "ExitCode": c.exit_code, "Running": c.status == "running"},
            "Config": {"Cmd": c.cmd}, "HostConfig": {"Binds": c.binds}})).into_response(),
        None => no_such(&id) }
}
async fn containers_json(State(a): State<App>) -> Json<Value> {
    let v: Vec<Value> = a.inner.lock().await.containers.values().map(|c| json!({
        "Id": c.id, "Image": c.image, "Command": c.cmd.join(" "),
        "State": c.status, "Status": c.status, "Names": [format!("/{}", &c.id[..12.min(c.id.len())])]})).collect();
    Json(json!(v))
}
async fn containers_delete(State(a): State<App>, Path(id): Path<String>) -> Response {
    if a.inner.lock().await.containers.remove(&id).is_some() { StatusCode::NO_CONTENT.into_response() } else { no_such(&id) }
}
fn no_such(id: &str) -> Response {
    (StatusCode::NOT_FOUND, Json(json!({"message": format!("No such container: {id}")}))).into_response()
}

/// Translate the container into a typed [`SpawnConfig`] and run it in the matching guest's JIT.
async fn run_in_jit(c: &Container) -> (Vec<u8>, Vec<u8>, i64) {
    let guest = c.arch.unwrap_or(Guest::Aarch64);
    let mut cfg = SpawnConfig::new(String::new(), c.rootfs.clone()); // absolute paths -> no work_dir cd
    cfg.argv = c.cmd.clone();
    cfg.hostname = (!c.hostname.is_empty()).then(|| c.hostname.clone());
    cfg.mem_max = c.memory.max(0) as u64;
    cfg.pids_max = c.pids_limit.max(0) as u32;
    cfg.netns = Some(c.id[..c.id.len().min(40)].to_string());
    cfg.volumes = c.binds.iter().filter_map(|b| b.split_once(':').map(|(host, dst)| Volume { container: dst.into(), host: host.into() })).collect();
    cfg.publish = c.publish.split(',').filter(|s| !s.is_empty()).filter_map(|p| p.split_once(':'))
        .filter_map(|(h, cc)| Some(PortMap { host: h.parse().ok()?, container: cc.parse().ok()? })).collect();

    let (prog, args) = match cfg.command(guest) {
        Some(x) => x,
        None => return (vec![], format!("no JIT built for guest arch {}\n", guest.as_str()).into_bytes(), 127),
    };
    match tokio::process::Command::new(prog).args(args).output().await {
        Ok(o) => (o.stdout, o.stderr, o.status.code().unwrap_or(-1) as i64),
        Err(e) => (vec![], format!("jit exec failed: {e}\n").into_bytes(), 127),
    }
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
        let arch = detect_arch(&rootfs).unwrap_or(Guest::Aarch64);
        out.push(Image { name, rootfs: rootfs.to_string_lossy().into_owned(), arch, cmd: vec!["/bin/sh".into()] });
    }
    out
}

/// Read the ELF e_machine of a likely executable in the rootfs to pick the guest arch.
fn detect_arch(rootfs: &std::path::Path) -> Option<Guest> {
    for probe in ["bin/busybox", "bin/sh", "bin/true", "usr/bin/coreutils"] {
        let p = rootfs.join(probe);
        if let Ok(b) = std::fs::read(&p) {
            if b.len() > 19 && &b[0..4] == b"\x7fELF" {
                return match u16::from_le_bytes([b[18], b[19]]) {  // e_machine
                    0xB7 => Some(Guest::Aarch64),
                    0x3E => Some(Guest::X86_64),
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

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
    http::{HeaderValue, StatusCode, Uri},
    response::{IntoResponse, Response},
    routing::{delete, get, post},
    Json, Router,
};
use ddjit::Guest;
use serde_json::json;
use std::sync::Arc;
use tokio::sync::Mutex;

mod registry;

mod model;
mod util;
mod system;
mod images;
mod containers;
mod build;
mod archive;
mod volumes;
mod networks;
mod runtime;
mod events;

use crate::model::*;
use crate::util::*;
use crate::system::*;
use crate::images::*;
use crate::containers::*;
use crate::build::*;
use crate::archive::*;
use crate::volumes::*;
use crate::networks::*;


/// Read-only bundled starter-image dirs to discover ALONGSIDE the writable `images_dir`: the app
/// bundle's `Resources/images`, a sibling of this daemon binary. We discover (not copy) them so an app
/// update always serves the current starter images and `~/.dd` never needs a manual refresh. Empty in a
/// dev/test tree (no such sibling exists next to the binary), so it can't perturb the matrix.
fn bundled_image_dirs(images_dir: &str) -> Vec<String> {
    let mut out = Vec::new();
    if let Ok(exe) = std::env::current_exe() {
        if let Some(dir) = exe.parent() {
            let p = dir.join("images");
            if p.is_dir() && p.to_string_lossy() != images_dir {
                out.push(p.to_string_lossy().into_owned());
            }
        }
    }
    out
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
    // Discover the writable user image store (DD_IMAGES = ~/.dd/images) PLUS any read-only bundled
    // starter images shipped inside the app (Resources/images, beside this binary). Serving the bundled
    // set straight from the bundle -- instead of copying it into ~/.dd -- means an app update always
    // carries the current starter images and nothing in ~/.dd ever needs refreshing. User pulls win on
    // a name clash.
    let mut imgs = discover_images(&images_dir);
    for d in bundled_image_dirs(&images_dir) {
        for img in discover_images(&d) {
            if !imgs.iter().any(|i| i.name == img.name) { imgs.push(img); }
        }
    }
    inner.images = imgs;
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
    let app = App { inner: Arc::new(Mutex::new(inner)), state_path, volumes_dir, images_dir, events: events::new_bus() };

    let router = Router::new()
        .route("/_ping", get(|| async { "OK" }))
        .route("/version", get(version)).route("/info", get(info))
        .route("/events", get(events::events))
        .route("/system/df", get(system_df))
        .route("/auth", post(auth))
        .route("/distribution/:name/json", get(distribution_inspect))
        .route("/images/json", get(images_json)).route("/images/create", post(images_create))
        .route("/images/get", get(image_save)).route("/images/load", post(image_load))
        .route("/images/search", get(image_search))
        .route("/images/prune", post(images_prune))
        .route("/images/:name/json", get(image_inspect))
        .route("/images/:name/history", get(image_history))
        .route("/images/:name/push", post(image_push))
        .route("/build", post(images_build))
        .route("/build/prune", post(build_prune))
        .route("/images/:name/tag", post(image_tag))
        .route("/images/:name", delete(image_delete))
        .route("/containers/json", get(containers_json))
        .route("/containers/create", post(containers_create))
        .route("/containers/prune", post(containers_prune))
        .route("/containers/:id/changes", get(containers_changes))
        .route("/containers/:id/export", get(containers_export))
        .route("/containers/:id/update", post(containers_update))
        .route("/containers/:id/start", post(containers_start))
        .route("/containers/:id/attach", post(containers_attach))
        .route("/containers/:id/stop", post(containers_stop))
        .route("/containers/:id/kill", post(containers_kill))
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
        .route("/commit", post(commit_container))
        .route("/volumes", get(volumes_list))
        .route("/volumes/create", post(volumes_create))
        .route("/volumes/prune", post(volumes_prune))
        .route("/volumes/:name", get(volume_inspect).delete(volume_delete))
        .route("/networks", get(networks_list))
        .route("/networks/create", post(networks_create))
        .route("/networks/prune", post(networks_prune))
        .route("/networks/:id", get(network_inspect).delete(network_delete))
        .route("/networks/:id/connect", post(network_connect))
        .route("/networks/:id/disconnect", post(network_disconnect))
        .fallback(not_found)
        // Every response carries Docker's negotiation/identity headers so the CLI's version
        // handshake and `docker version`/`info` work without falling back to defaults.
        .layer(axum::middleware::map_response(docker_headers))
        // A Docker daemon ingests large tarball bodies (build contexts, `docker load`, `docker cp`),
        // which exceed axum's 2MB default Bytes-extractor limit -> disable it.
        .layer(axum::extract::DefaultBodyLimit::disable())
        .with_state(app);

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
    // collapse a NAMESPACED image reference into a single path segment so the `:name` routes match.
    // docker addresses image endpoints as /images/<ref>[/<verb>] where <ref> may embed slashes
    // (registry/ns/name:tag), e.g. POST /images/docker.io/library/ubuntu/push.
    pq = normalize_image_path(&pq);
    if let Ok(uri) = pq.parse::<Uri>() { *req.uri_mut() = uri; }
}

/// Rewrite an image-reference path so axum's single-segment `:name` capture can match a NAMESPACED
/// and/or tagged reference. docker addresses image endpoints as `/images/<ref>[/<verb>]` where `<ref>`
/// may embed slashes (`registry/ns/name:tag`); `:name` only captures one segment, so we collapse the
/// reference's internal slashes to `%2F` (axum's `Path` extractor percent-decodes them back to the full
/// ref, and `matchit` matches `%2F` as literal chars rather than a separator). Verb sub-resources
/// (`json`/`history`/`tag`/`push`) and the bare `DELETE /images/<ref>` are all handled; the fixed
/// `/images/{json,create,get,load,search,prune}` endpoints carry no embedded ref and pass through.
fn normalize_image_path(pq: &str) -> String {
    let (path, query) = pq.split_once('?').map(|(p, q)| (p, Some(q))).unwrap_or((pq, None));
    let rebuild = |p: String| match query { Some(q) => format!("{p}?{q}"), None => p };
    let Some(rest) = path.strip_prefix("/images/") else { return pq.to_string() };
    let mut segs: Vec<&str> = rest.split('/').collect();
    // Peel off a trailing verb sub-resource; whatever remains is the (possibly multi-segment) reference.
    let verb = matches!(segs.last().copied(), Some("json" | "history" | "tag" | "push"))
        .then(|| segs.pop().unwrap());
    let reference = segs.join("/");
    // No reference means a fixed endpoint (`/images/json` list, `/images/create`, …) — leave it alone.
    if reference.is_empty() { return pq.to_string(); }
    let encoded = reference.replace('/', "%2F");
    rebuild(match verb {
        Some(v) => format!("/images/{encoded}/{v}"),
        None => format!("/images/{encoded}"),
    })
}


async fn not_found(uri: Uri) -> Response {
    (StatusCode::NOT_FOUND, Json(json!({"message": format!("no route for {uri}")}))).into_response()
}

/// Stamp Docker's negotiation + identity headers onto every response. The `docker` CLI reads
/// `Api-Version`/`Ostype` from these to negotiate, and tools probe `Server`/`Docker-Experimental`.
async fn docker_headers(mut resp: Response) -> Response {
    let h = resp.headers_mut();
    h.insert("Api-Version", HeaderValue::from_static(API_VERSION));
    h.insert("Ostype", HeaderValue::from_static("linux"));
    h.insert("Docker-Experimental", HeaderValue::from_static("false"));
    h.insert("Builder-Version", HeaderValue::from_static("1"));
    h.insert("Server", HeaderValue::from_static("dd-daemon/0.1.0"));
    h.insert("Cache-Control", HeaderValue::from_static("no-cache, no-store, must-revalidate"));
    h.insert("Pragma", HeaderValue::from_static("no-cache"));
    resp
}

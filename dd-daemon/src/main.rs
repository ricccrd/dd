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

use crate::model::*;
use crate::util::*;
use crate::system::*;
use crate::images::*;
use crate::containers::*;
use crate::build::*;
use crate::archive::*;
use crate::volumes::*;
use crate::networks::*;


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
        .route("/events", get(events))
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

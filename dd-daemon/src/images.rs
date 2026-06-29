#![allow(unused_imports, dead_code)]
use crate::model::*;
use crate::util::*;
use crate::system::*;
use crate::containers::*;
use crate::build::*;
use crate::archive::*;
use crate::volumes::*;
use crate::networks::*;
use crate::runtime::*;
use crate::registry::{Client, Credentials, ImageRef, PullEvent, layer_short};
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

pub(crate) async fn images_json(State(a): State<App>) -> Json<Value> {
    let imgs: Vec<Value> = a.inner.lock().await.images.iter().map(|i| {
        let size = image_size(&i.rootfs, &i.name);
        json!({
        "Id": format!("sha256:{}", fake_id(&i.name)), "RepoTags": [repo_tag(&i.name)],
        "Created": i.created, "Size": size,
        // Fields required by the Docker `ImageSummary` schema (strict clients like bollard reject the
        // object if any are absent). `VirtualSize` is a required i64 in API <=1.43 models (no serde
        // default), so it must be present; dd has no parent/registry-digest/shared-size accounting yet,
        // so the rest take the Docker "not calculated" sentinels (-1) or empties.
        "VirtualSize": size, "ParentId": "", "RepoDigests": [], "SharedSize": -1, "Labels": i.labels, "Containers": -1})
    }).collect();
    Json(json!(imgs))
}

/// `GET /images/{name}/history` — `docker history`. dd squashes images to a single rootfs, so we
/// report one synthetic layer.
pub(crate) async fn image_history(State(a): State<App>, Path(name): Path<String>) -> Response {
    let g = a.inner.lock().await;
    match find_image(&g.images, &name) {
        Some(i) => Json(json!([{
            "Id": format!("sha256:{}", fake_id(&i.name)), "Created": i.created,
            "CreatedBy": "dd import", "Tags": [repo_tag(&i.name)],
            "Size": image_size(&i.rootfs, &i.name), "Comment": ""}])).into_response(),
        None => (StatusCode::NOT_FOUND, Json(json!({"message": format!("No such image: {name}")}))).into_response(),
    }
}

/// `GET /images/search` — `docker search`. dd has no search index; return an empty result set with
/// the correct shape rather than 404.
pub(crate) async fn image_search() -> Json<Value> {
    Json(json!([]))
}

/// `POST /images/prune` — `docker image prune`. dd does not track dangling images; report nothing
/// reclaimed (correct shape so `docker system prune` succeeds).
pub(crate) async fn images_prune() -> Json<Value> {
    Json(json!({"ImagesDeleted": [], "SpaceReclaimed": 0}))
}

/// `GET /distribution/{name}/json` — registry manifest probe. Minimal conformant descriptor.
pub(crate) async fn distribution_inspect(Path(name): Path<String>) -> Response {
    Json(json!({
        "Descriptor": {"mediaType": "application/vnd.docker.distribution.manifest.v2+json",
            "digest": format!("sha256:{}", fake_id(&name)), "size": 0},
        "Platforms": [{"architecture": "arm64", "os": "linux"}]})).into_response()
}

/// GET /images/:name/json — `docker image inspect` / `docker run`'s local-image probe. Returns the
/// image config (Cmd/Entrypoint/Env) so the CLI doesn't treat the image as missing and re-pull.
pub(crate) async fn image_inspect(State(a): State<App>, Path(name): Path<String>) -> Response {
    // On a miss, re-scan the images dir from disk before reporting 404: the image may be on disk
    // (freshly pulled/built) yet absent from the in-memory store.
    if find_image(&a.inner.lock().await.images, &name).is_none() {
        rescan_images(&a).await;
    }
    let g = a.inner.lock().await;
    match find_image(&g.images, &name) {
        Some(i) => {
            let tag = repo_tag(&i.name);
            let size = image_size(&i.rootfs, &i.name);
            // The image stores ENTRYPOINT separately; Docker reports a missing entrypoint as null
            // (not []), and `docker inspect` clients distinguish the two.
            let entrypoint = if i.entrypoint.is_empty() { Value::Null } else { json!(i.entrypoint) };
            // Use the image's recorded ENV; fall back to a sane PATH so containers run by a client
            // that copies Config.Env verbatim still resolve binaries.
            let env: Vec<String> = if i.env.is_empty() {
                vec!["PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin".into()]
            } else { i.env.clone() };
            Json(json!({
                "Id": format!("sha256:{}", fake_id(&i.name)),
                "RepoTags": [tag.clone()], "RepoDigests": [],
                "Architecture": docker_arch(i.arch),
                "Os": i.arch.os(),
                // RFC3339 string shape strict clients (bollard) expect; `created` is unix secs.
                "Size": size, "VirtualSize": size, "Created": fmt_rfc3339(i.created),
                "Config": {
                    "Image": tag,
                    "Cmd": i.cmd,
                    "Entrypoint": entrypoint,
                    "Env": env,
                    "WorkingDir": i.workdir,
                    "Labels": i.labels,
                },
                "RootFS": {"Type": "layers", "Layers": []}})).into_response()
        }
        None => (StatusCode::NOT_FOUND, Json(json!({"message": format!("No such image: {name}")}))).into_response(),
    }
}

#[derive(Deserialize)]
pub(crate) struct ImageCreateQ {
    #[serde(rename = "fromImage")] from_image: Option<String>,
    #[serde(rename = "fromSrc")] from_src: Option<String>,
    repo: Option<String>,
    #[serde(rename = "tag")] tag: Option<String>,
    platform: Option<String>,
}

/// POST /images/create -- `docker pull` (when `fromImage` is set) or `docker import` (when `fromSrc`
/// is set). For a pull: if the image isn't local (for the requested platform), pull it from its
/// registry (any registry) and unpack it into a rootfs, then register it. For an import: extract the
/// rootfs tar from the request body into a new image named by `repo`.
pub(crate) async fn images_create(State(a): State<App>, Query(q): Query<ImageCreateQ>, headers: axum::http::HeaderMap, body: axum::body::Bytes) -> Response {
    // `docker import` routes through this same endpoint but carries `fromSrc` (the rootfs source)
    // instead of `fromImage`; dispatch it to the import path before the pull logic.
    if let Some(src) = q.from_src.clone().filter(|s| !s.is_empty()) {
        return image_import(a, q.repo.clone().unwrap_or_default(), q.tag.clone().unwrap_or_default(), &src, body).await;
    }
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
        return pull_progress(&name, &tag, Ok(true), "", 0);
    }
    let creds = registry_auth(&headers);
    let archs = platform_archs(q.platform.as_deref());
    pull_stream(a, name, tag, want, creds, archs)
}

/// Stream a fresh `docker pull` as newline-delimited JSON, flushing each status line as the download
/// proceeds (mirrors the `events.rs` streamed-body pattern). A background task drives the blocking
/// registry pull, forwarding its per-layer [`PullEvent`]s into the response body live; on completion it
/// registers the image and emits the closing `Digest:`/`Status:` lines (or an error line). This replaces
/// the old "block until done, then dump a fixed sequence" behavior so the client renders moving bars.
fn pull_stream(a: App, name: String, tag: String, want: String, creds: Credentials, archs: Vec<&'static str>) -> Response {
    // Lines flow out through `line_rx`; the body stream just drains it (closed when the worker drops tx).
    // An awaited `send` gives natural backpressure (a slow/stalled client throttles the producer rather
    // than silently dropping lines); a send error just means the client hung up — stop quietly.
    let (line_tx, line_rx) = mpsc::channel::<Vec<u8>>(256);
    tokio::spawn(async move {
        macro_rules! emit { ($v:expr) => {
            if line_tx.send(($v.to_string() + "\r\n").into_bytes()).await.is_err() { return; }
        }; }
        let repo = image_ref(&name, &tag).repository;
        emit!(json!({ "status": format!("Pulling from {repo}"), "id": tag }));
        // The blocking pull reports progress over `pev`; forward+format each event into a status line.
        let (pev_tx, mut pev_rx) = mpsc::channel::<PullEvent>(256);
        let (dir, nm, tg) = (a.images_dir.clone(), name.clone(), tag.clone());
        let blocking = tokio::task::spawn_blocking(move || {
            let mut cb = |e: PullEvent| { let _ = pev_tx.blocking_send(e); };
            pull_image(&dir, &nm, &tg, creds, &archs, &mut cb)
        });
        while let Some(e) = pev_rx.recv().await {
            emit!(pull_event_json(&e));
        }
        let res = blocking.await.unwrap_or_else(|e| Err(format!("pull task crashed: {e}")));
        match res {
            Ok(img) => {
                let digest = format!("sha256:{}", fake_id(&img.name));
                {
                    let mut g = a.inner.lock().await;
                    g.images.retain(|i| repo_tag(&i.name) != want); // a re-pull (new platform) replaces the old
                    g.images.push(img);
                }
                crate::events::emit_event(&a.events, "image", "pull", &want, json!({"name": want}));
                emit!(json!({ "status": format!("Digest: {digest}") }));
                emit!(json!({ "status": format!("Status: Downloaded newer image for {name}:{tag}") }));
            }
            Err(e) => emit!(json!({ "errorDetail": { "message": e.clone() }, "error": e })),
        }
    });
    let body = futures_util::stream::unfold(line_rx, |mut rx| async move {
        rx.recv().await.map(|b| (Ok::<Vec<u8>, std::io::Error>(b), rx))
    });
    Response::builder().status(StatusCode::OK).header("Content-Type", "application/json")
        .body(Body::from_stream(body)).unwrap()
}

/// Format one live [`PullEvent`] into the docker-shaped JSON status object the CLI renders as a bar.
fn pull_event_json(e: &PullEvent) -> Value {
    match e {
        PullEvent::Layer { id } => json!({ "status": "Pulling fs layer", "id": id }),
        PullEvent::Downloading { id, current, total } =>
            json!({ "status": "Downloading", "progressDetail": { "current": current, "total": total }, "id": id }),
        PullEvent::DownloadComplete { id } => json!({ "status": "Download complete", "id": id }),
        PullEvent::Extracting { id, current, total } =>
            json!({ "status": "Extracting", "progressDetail": { "current": current, "total": total }, "id": id }),
        PullEvent::PullComplete { id } => json!({ "status": "Pull complete", "id": id }),
    }
}


/// Decode the CLI's `X-Registry-Auth` header (base64 JSON credentials) into [`Credentials`].
pub(crate) fn registry_auth(headers: &axum::http::HeaderMap) -> Credentials {
    headers.get("X-Registry-Auth").and_then(|v| v.to_str().ok())
        .and_then(Credentials::from_x_registry_auth).unwrap_or_default()
}


/// docker-style pull progress: a newline-delimited stream of JSON status lines the CLI renders.
///
/// `digest`/`size` describe the pulled image (its synthetic content digest and on-disk rootfs size);
/// they drive a docker-shaped per-layer progress sequence on a fresh pull. dd squashes an image to a
/// single rootfs, so we surface ONE synthetic layer (id = first 12 hex of the digest) rather than the
/// registry's real per-blob layers. See the registry note in the push helper for what real byte
/// progress would require.
pub(crate) fn pull_progress(name: &str, tag: &str, result: Result<bool, String>, digest: &str, size: i64) -> Response {
    let body = match result {
        Ok(true) => format!("{}\r\n", json!({ "status": format!("Status: Image is up to date for {name}:{tag}") })),
        Ok(false) => {
            let repo = image_ref(name, tag).repository;
            let layer_id = digest.trim_start_matches("sha256:").chars().take(12).collect::<String>();
            let layer = layer_id.as_str();
            let half = (size / 2).max(0);
            [
                json!({ "status": format!("Pulling from {repo}"), "id": tag }).to_string(),
                json!({ "status": "Pulling fs layer", "id": layer }).to_string(),
                json!({ "status": "Downloading", "progressDetail": { "current": half, "total": size }, "id": layer }).to_string(),
                json!({ "status": "Downloading", "progressDetail": { "current": size, "total": size }, "id": layer }).to_string(),
                json!({ "status": "Verifying Checksum", "id": layer }).to_string(),
                json!({ "status": "Download complete", "id": layer }).to_string(),
                json!({ "status": "Extracting", "progressDetail": { "current": size, "total": size }, "id": layer }).to_string(),
                json!({ "status": "Pull complete", "id": layer }).to_string(),
                json!({ "status": format!("Digest: {digest}") }).to_string(),
                json!({ "status": format!("Status: Downloaded newer image for {name}:{tag}") }).to_string(),
            ].join("\r\n") + "\r\n"
        }
        Err(e) => json!({ "errorDetail": { "message": e.clone() }, "error": e }).to_string() + "\r\n",
    };
    (StatusCode::OK, [("Content-Type", "application/json")], body).into_response()
}


/// Build an [`ImageRef`] from docker's separate `fromImage` + `tag` params (tag overrides any in the ref).
pub(crate) fn image_ref(from_image: &str, tag: &str) -> ImageRef {
    let mut r = ImageRef::parse(from_image);
    if !tag.is_empty() { r.tag = tag.to_string(); }
    r
}


/// Pull an image from its registry (any registry) and unpack it under `<images_dir>/<safe>/rootfs`,
/// preferring the linux/arm64 variant (native; falls back to amd64). Returns the registered [`Image`].
pub(crate) fn pull_image(images_dir: &str, from_image: &str, tag: &str, creds: Credentials, archs: &[&str], progress: &mut dyn FnMut(PullEvent)) -> Result<Image, String> {
    let iref = image_ref(from_image, tag);
    let rootfs = std::path::PathBuf::from(format!("{images_dir}/{}/rootfs", safe_name(&iref)));
    let pulled = Client::new(iref.clone(), creds).pull(&rootfs, archs, progress)?;
    // Distroless/scratch images carry no ELF/Mach-O to sniff, so the rootfs scan comes up empty.
    // Fall back to the manifest config's `architecture`+`os`, then to native arm64 — never fail the
    // pull just because the arch couldn't be detected.
    let arch = detect_arch(&rootfs)
        .or_else(|| manifest_arch(&pulled.config))
        .unwrap_or(Guest::LinuxAarch64);
    let darwin = arch.os() == "darwin";
    // Pull the OCI config's full run metadata. Entrypoint and Cmd are kept *separate* (NOT flattened like
    // `config_cmd` does) so docker's override semantics survive the round-trip — `containers_create` rebuilds
    // argv = entrypoint ++ cmd, and `--entrypoint`/CMD overrides act on the right half (see containers.rs).
    let entrypoint = config_strs(&pulled.config, "Entrypoint");
    let env = config_strs(&pulled.config, "Env");
    let workdir = pulled.config["config"]["WorkingDir"].as_str().unwrap_or("").to_string();
    let labels = config_labels(&pulled.config);
    // A pulled macOS image's `dd-image.json` sidecar doesn't survive the registry round-trip and its
    // userland shell lives on the in-jail PATH (`/profile/bin/bash`), not `/bin/sh` — so default a
    // darwin image to a bare `bash` (resolved via PATH by the darwinjail) rather than `/bin/sh`. Only fall
    // back when the config supplies neither Entrypoint nor Cmd (an entrypoint-only image keeps empty cmd).
    let mut cmd = config_strs(&pulled.config, "Cmd");
    if cmd.is_empty() && entrypoint.is_empty() {
        cmd = if darwin { vec!["bash".into()] } else { default_shell(&rootfs) };
    }
    let name = iref.short();
    // Record name + the full OCI run config (cmd/env/entrypoint/workdir, +os for darwin) so the image keeps
    // its identity AND its entrypoint/env/workdir across a daemon restart (the dir name alone doesn't
    // round-trip -- e.g. "docker.io_library_alpine_latest"). Mirrors the `docker load` path (`image_load`).
    let mut meta = json!({ "name": name.clone(), "cmd": cmd.clone(), "env": env.clone(),
                           "entrypoint": entrypoint.clone(), "workdir": workdir.clone(),
                           "arch": arch.arch(), "os": arch.os() });
    if darwin { meta["os"] = json!("darwin"); }
    let _ = std::fs::write(format!("{images_dir}/{}/dd-image.json", safe_name(&iref)), meta.to_string());
    Ok(Image {
        name, rootfs: rootfs.to_string_lossy().into_owned(), arch,
        cmd, env, entrypoint, workdir, labels, created: now_secs(),
    })
}


/// Derive a [`Guest`] from an OCI/Docker image config's top-level `architecture`+`os`
/// (e.g. `{"architecture":"amd64","os":"linux"}`). Used as a fallback when the rootfs has no
/// ELF/Mach-O to sniff (distroless/scratch). Returns `None` when the fields are absent/unrecognized.
fn manifest_arch(config: &Value) -> Option<Guest> {
    let os = config["os"].as_str().unwrap_or("linux");
    match (os, config["architecture"].as_str()?) {
        ("darwin", "arm64" | "aarch64") => Some(Guest::DarwinAarch64),
        (_, "amd64" | "x86_64") => Some(Guest::LinuxX86_64),
        (_, "arm64" | "aarch64") => Some(Guest::LinuxAarch64),
        _ => None,
    }
}


/// A `repository:tag` string with exactly one tag — discovered images carry a bare name (`busybox`),
/// pulled ones already include the tag (`busybox:latest`); append `:latest` only when absent.
pub(crate) fn repo_tag(name: &str) -> String {
    let last = name.rsplit('/').next().unwrap_or(name);
    if last.contains(':') { name.to_string() } else { format!("{name}:latest") }
}

/// The tag portion of an image reference, defaulting to `latest` when none is given. A `:port` inside a
/// registry host (`localhost:5000/foo`) is NOT a tag — only a final `:tag` with no slash after the colon
/// counts: `ubuntu:24.04` -> `24.04`, `ubuntu` -> `latest`, `localhost:5000/foo` -> `latest`. Lets `rmi`
/// (and `push`) tell `ubuntu:24.04` apart from `ubuntu` (`:latest`) so an untag is tag-precise.
pub(crate) fn ref_tag(name: &str) -> String {
    match name.rsplit_once(':') {
        Some((_, t)) if !t.contains('/') => t.to_string(),
        _ => "latest".to_string(),
    }
}


/// A filesystem-safe directory name for an image reference.
pub(crate) fn safe_name(r: &ImageRef) -> String { r.canonical().replace(['/', ':'], "_") }


/// A string array at `config.config.<key>` of an OCI image config blob (e.g. `Entrypoint`, `Cmd`, `Env`),
/// flattened to `Vec<String>` (non-string/absent -> empty). Kept granular so pull can persist Entrypoint
/// and Cmd separately (docker's override semantics depend on the split).
pub(crate) fn config_strs(config: &Value, key: &str) -> Vec<String> {
    config["config"][key].as_array()
        .map(|a| a.iter().filter_map(|x| x.as_str().map(String::from)).collect())
        .unwrap_or_default()
}

/// The `config.config.Labels` object of an OCI image config blob as a `HashMap` (absent/non-string -> empty).
pub(crate) fn config_labels(config: &Value) -> std::collections::HashMap<String, String> {
    config["config"]["Labels"].as_object()
        .map(|m| m.iter().filter_map(|(k, v)| v.as_str().map(|s| (k.clone(), s.to_string()))).collect())
        .unwrap_or_default()
}

/// Fallback default command for an image whose config has no Cmd: prefer /bin/sh, else /bin/bash.
pub(crate) fn default_shell(rootfs: &std::path::Path) -> Vec<String> {
    for sh in ["/bin/sh", "/bin/bash"] {
        if rootfs.join(sh.trim_start_matches('/')).exists() { return vec![sh.to_string()]; }
    }
    vec!["/bin/sh".to_string()]
}


// ---- image management: tag / rmi / push -------------------------------------
#[derive(Deserialize)]
pub(crate) struct TagQ { repo: Option<String>, tag: Option<String> }

/// POST /images/:name/tag -- alias an image under a new repo[:tag] (same rootfs). Honors both the
/// `repo` and `tag` query params (`docker tag src dst:v2` -> repo=dst, tag=v2).
pub(crate) async fn image_tag(State(a): State<App>, Path(name): Path<String>, Query(q): Query<TagQ>) -> Response {
    let mut g = a.inner.lock().await;
    let Some(src) = find_image(&g.images, &name).cloned() else {
        return (StatusCode::NOT_FOUND, Json(json!({"message": format!("No such image: {name}")}))).into_response();
    };
    // Keep the FULL target repository (registry + namespace), e.g. `huttarichard/ddmac` — NOT the bare
    // name. Stripping it (ref_name) would later push to `library/<name>` and be denied. docker sends the
    // repo without a tag and the tag separately.
    let repo = q.repo.unwrap_or_default();
    if repo.is_empty() { return (StatusCode::BAD_REQUEST, Json(json!({"message": "repo required"}))).into_response(); }
    let full = match q.tag.filter(|t| !t.is_empty()) { Some(t) => format!("{repo}:{t}"), None => repo };
    if !g.images.iter().any(|i| i.name == full) {
        g.images.push(Image { name: full.clone(), ..src });
    }
    crate::events::emit_event(&a.events, "image", "tag", &full, json!({"name": full}));
    StatusCode::CREATED.into_response()
}

/// DELETE /images/:name -- `docker rmi`. Tag-precise, matching Docker semantics: `rmi <name>:<tag>`
/// (or bare `<name>`, which means `<name>:latest`) removes ONLY that one tag entry from the store. The
/// on-disk rootfs is deleted only when this was its LAST reference; if another tag (a `docker tag` alias)
/// still points at the same rootfs we just drop the tag (an untag) and keep the layers. So `rmi ubuntu`
/// with `ubuntu:24.04` also present untags only `ubuntu:latest` and leaves `ubuntu:24.04` resolvable.
pub(crate) async fn image_delete(State(a): State<App>, Path(name): Path<String>) -> Response {
    let mut g = a.inner.lock().await;
    let (want_repo, want_tag) = (ref_name(&name).to_string(), ref_tag(&name));
    // The single tag entry the reference names (repository AND tag must match). `ref_name`/`ref_tag`
    // mirror the lenient matching used elsewhere (registry/namespace ignored).
    let matches = |i: &Image| ref_name(&i.name) == want_repo && ref_tag(&i.name) == want_tag;
    let Some(target) = g.images.iter().find(|i| matches(i)).cloned() else {
        return (StatusCode::NOT_FOUND, Json(json!({"message": format!("No such image: {name}")}))).into_response();
    };
    let untagged = repo_tag(&target.name);
    g.images.retain(|i| !matches(i)); // remove only this tag, never sibling tags of the same repo
    // Delete the on-disk rootfs only when this was its last reference: another tag sharing the same
    // rootfs (a `docker tag` alias) keeps it alive, so we report an untag and leave the layers in place.
    let last_ref = !g.images.iter().any(|i| i.rootfs == target.rootfs);
    let mut report = vec![json!({ "Untagged": untagged })];
    if last_ref && target.name != "macos" { // the host `macos` image's rootfs is the live `/` — never delete
        remove_image_dir(&a.images_dir, &target.rootfs);
        report.push(json!({ "Deleted": format!("sha256:{}", fake_id(&target.name)) }));
    }
    crate::events::emit_event(&a.events, "image", "delete", &want_repo, json!({"name": repo_tag(&target.name)}));
    Json(json!(report)).into_response()
}

/// Remove an image's on-disk directory (`<images_dir>/<safe>/`, the parent of its `rootfs/`). Guarded
/// to the writable image store: a rootfs under a read-only bundled starter dir (or anywhere outside
/// `images_dir`) is left untouched so `rmi` of a discovered alias can't wipe shipped images.
fn remove_image_dir(images_dir: &str, rootfs: &str) {
    let Some(dir) = std::path::Path::new(rootfs).parent() else { return };
    let base = std::path::Path::new(images_dir);
    if dir != base && dir.starts_with(base) {
        let _ = std::fs::remove_dir_all(dir);
    }
}

/// POST /images/:name/push -- dd has no registry yet (images are local), so this is a clean no-op that
/// reports success rather than erroring. TODO(OCI): real registry push.
/// POST /images/:name/push -- re-tar the local rootfs into a single-layer image and upload it to its
/// registry (`docker.io/...`, `ghcr.io/...`, `localhost:5000/...`) using the CLI's credentials.
pub(crate) async fn image_push(State(a): State<App>, Path(name): Path<String>, Query(q): Query<PushQ>, headers: axum::http::HeaderMap) -> Response {
    // The route `name` is collapsed to the bare image (e.g. `huttarichard/ddmac` -> `ddmac`), so match on
    // it AND the requested tag, then push to the image's FULL stored name so the registry namespace
    // (`huttarichard/…`) is preserved — otherwise the upload targets `library/<name>` and is denied.
    let want_tag = q.tag.filter(|t| !t.is_empty()).unwrap_or_else(|| "latest".into());
    let img = {
        let g = a.inner.lock().await;
        g.images.iter().find(|i| ref_name(&i.name) == ref_name(&name) && ref_tag(&i.name) == want_tag)
            .or_else(|| g.images.iter().find(|i| ref_name(&i.name) == ref_name(&name)))
            .cloned()
    };
    let Some(img) = img else {
        return push_progress(&name, &want_tag, 0, Err(format!("No such image: {name}"))).into_response();
    };
    let tag = want_tag;
    let iref = image_ref(&img.name, &tag);
    let arch = docker_arch(img.arch).to_string();
    let os = img.arch.os().to_string(); // "darwin" for mac images, else "linux"
    let creds = registry_auth(&headers);
    // On-disk rootfs size, captured before `img` is moved into the push task; reported as the layer
    // `Size` in the push progress/aux lines (a real registry manifest size would need registry.rs to
    // surface it — see note below).
    let size = image_size(&img.rootfs, &img.name);
    let work = std::path::PathBuf::from(format!("{}/.push-{}", a.images_dir, std::process::id()));
    let res = tokio::task::spawn_blocking(move || {
        Client::new(iref, creds).push(std::path::Path::new(&img.rootfs), &img.cmd, &arch, &os, &work)
    }).await.unwrap_or_else(|e| Err(format!("push task crashed: {e}")));
    push_progress(&name, &tag, size, res).into_response()
}


#[derive(Deserialize)]
pub(crate) struct PushQ { tag: Option<String> }

/// docker arch label for a guest target.
pub(crate) fn docker_arch(g: Guest) -> &'static str { if g.arch() == "x86_64" { "amd64" } else { "arm64" } }

/// A docker `--platform` value ("linux/amd64", "arm64", …) mapped to dd's arch label, if recognized.
pub(crate) fn platform_arch(platform: Option<&str>) -> Option<&'static str> {
    match platform?.rsplit('/').next().unwrap_or("") {
        "amd64" | "x86_64" => Some("amd64"),
        "arm64" | "aarch64" => Some("arm64"),
        _ => None,
    }
}

/// Preferred arch list when pulling for a given platform: the requested one, else native-arm64 first.
pub(crate) fn platform_archs(platform: Option<&str>) -> Vec<&'static str> {
    match platform_arch(platform) { Some(a) => vec![a], None => vec!["arm64", "amd64"] }
}


/// docker-style push progress: a newline-delimited stream of JSON status lines (or an error line).
///
/// `digest` is the manifest digest returned by `Client::push` (the registry's `Docker-Content-Digest`),
/// `size` is the image's on-disk rootfs size used as the layer/aux `Size`. The stream ends with the
/// `aux` line (which the docker CLI parses to print `digest: … size: …`) followed by the matching
/// status line, so `docker push` reports the real pushed digest instead of a hardcoded `latest:`.
///
/// REPORT-only: a fully accurate `Size` would be the manifest byte length, not the rootfs size. The
/// registry client computes both the layer size and the manifest bytes internally; if `Client::push`
/// returned `(digest, manifest_size, layer_size)` instead of just the digest, dd could emit Docker's
/// exact `size:` value and real per-blob byte progress here.
pub(crate) fn push_progress(name: &str, tag: &str, size: i64, result: Result<String, String>) -> Response {
    let body = match result {
        Ok(digest) => {
            let layer_id = digest.trim_start_matches("sha256:").chars().take(12).collect::<String>();
            let layer = layer_id.as_str();
            let half = (size / 2).max(0);
            [
                json!({ "status": format!("The push refers to repository [{name}]") }).to_string(),
                json!({ "status": "Preparing", "id": layer }).to_string(),
                json!({ "status": "Pushing", "progressDetail": { "current": half, "total": size }, "id": layer }).to_string(),
                json!({ "status": "Pushing", "progressDetail": { "current": size, "total": size }, "id": layer }).to_string(),
                json!({ "status": "Pushed", "id": layer }).to_string(),
                json!({ "progressDetail": {}, "aux": { "Tag": tag, "Digest": digest.as_str(), "Size": size } }).to_string(),
                json!({ "status": format!("{tag}: digest: {digest} size: {size}") }).to_string(),
            ].join("\r\n") + "\r\n"
        }
        Err(e) => json!({ "errorDetail": { "message": e.clone() }, "error": e }).to_string() + "\r\n",
    };
    (StatusCode::OK, [("Content-Type", "application/json")], body).into_response()
}


// ---- image save / load / import --------------------------------------------
//
// dd's archive format is intentionally simple (not full OCI): a tar whose top level is the image's
// `rootfs/` directory plus a `dd-manifest.json` sidecar recording the image identity (name + run
// config). `docker save` produces it, `docker load` consumes it; `docker import` instead takes a
// bare rootfs tar (no manifest) whose files land directly in a new image's rootfs.

#[derive(Deserialize)]
pub(crate) struct SaveQ { names: Option<String> }

/// GET /images/get?names=<name> -- `docker save`. Streams a tar of the image's `rootfs/` directory
/// plus a `dd-manifest.json` naming the image, as `application/x-tar`.
pub(crate) async fn image_save(State(a): State<App>, Query(q): Query<SaveQ>) -> Response {
    let names = q.names.unwrap_or_default();
    if names.is_empty() {
        return (StatusCode::BAD_REQUEST, Json(json!({"message": "names is required"}))).into_response();
    }
    let img = {
        let g = a.inner.lock().await;
        g.images.iter().find(|i| repo_tag(&i.name) == names || ref_name(&i.name) == ref_name(&names)).cloned()
    };
    let Some(img) = img else {
        return (StatusCode::NOT_FOUND, Json(json!({"message": format!("No such image: {names}")}))).into_response();
    };
    // The `macos` image is the live host filesystem (rootfs ~ `/`); taring it would be catastrophic.
    if img.name == "macos" {
        return (StatusCode::BAD_REQUEST, Json(json!({"message": "cannot save the host `macos` image"}))).into_response();
    }
    let rootfs = std::path::PathBuf::from(&img.rootfs);
    let Some(parent) = rootfs.parent() else {
        return (StatusCode::INTERNAL_SERVER_ERROR, Json(json!({"message": "image has no rootfs directory"}))).into_response();
    };
    // Stage the manifest in a temp dir and tar it via a second `-C` so the on-disk image directory is
    // left untouched (and a later `docker load` can restore name/cmd/env exactly).
    let staging = std::env::temp_dir().join(format!("dd-save-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&staging);
    if let Err(e) = std::fs::create_dir_all(&staging) {
        return (StatusCode::INTERNAL_SERVER_ERROR, Json(json!({"message": e.to_string()}))).into_response();
    }
    let mut meta = json!({ "name": img.name, "cmd": img.cmd, "env": img.env, "entrypoint": img.entrypoint, "workdir": img.workdir });
    if img.arch.os() == "darwin" { meta["os"] = json!("darwin"); }
    let _ = std::fs::write(staging.join("dd-manifest.json"), meta.to_string());
    let out = std::process::Command::new("tar").arg("cf").arg("-")
        .arg("-C").arg(parent).arg("rootfs")
        .arg("-C").arg(&staging).arg("dd-manifest.json").output();
    let _ = std::fs::remove_dir_all(&staging);
    match out {
        Ok(o) if o.status.success() =>
            Response::builder().status(StatusCode::OK).header("Content-Type", "application/x-tar")
                .body(Body::from(o.stdout)).unwrap(),
        Ok(o) => (StatusCode::INTERNAL_SERVER_ERROR, Json(json!({"message": String::from_utf8_lossy(&o.stderr).into_owned()}))).into_response(),
        Err(e) => (StatusCode::INTERNAL_SERVER_ERROR, Json(json!({"message": e.to_string()}))).into_response(),
    }
}

/// POST /images/load -- `docker load`. Extracts a dd save archive (rootfs/ + dd-manifest.json) from
/// the request body into a new image directory and registers the image.
pub(crate) async fn image_load(State(a): State<App>, body: axum::body::Bytes) -> Response {
    let tmp = std::env::temp_dir().join(format!("dd-load-{}.tar", std::process::id()));
    if let Err(e) = std::fs::write(&tmp, &body) { return load_err(e.to_string()); }
    // Extract into a staging dir under DD_IMAGES (same filesystem) so we can rename it into place once
    // we've read the image name out of the manifest.
    let staging = std::path::PathBuf::from(format!("{}/.load-{}", a.images_dir, std::process::id()));
    let _ = std::fs::remove_dir_all(&staging);
    if let Err(e) = std::fs::create_dir_all(&staging) { let _ = std::fs::remove_file(&tmp); return load_err(e.to_string()); }
    let out = std::process::Command::new("tar").arg("xf").arg(&tmp).arg("-C").arg(&staging).output();
    let _ = std::fs::remove_file(&tmp);
    match out {
        Ok(o) if o.status.success() => {}
        Ok(o) => { let _ = std::fs::remove_dir_all(&staging); return load_err(String::from_utf8_lossy(&o.stderr).into_owned()); }
        Err(e) => { let _ = std::fs::remove_dir_all(&staging); return load_err(e.to_string()); }
    }
    if !staging.join("rootfs").is_dir() {
        let _ = std::fs::remove_dir_all(&staging);
        return load_err("archive is not a dd image (no rootfs/ at top level)".into());
    }
    // dd-manifest.json (written by `docker save`) carries the image identity; tolerate a rootfs-only
    // archive by falling back to a generic name.
    let meta = std::fs::read_to_string(staging.join("dd-manifest.json")).ok()
        .and_then(|s| serde_json::from_str::<Value>(&s).ok());
    let strs = |k: &str| meta.as_ref().and_then(|m| m[k].as_array())
        .map(|a| a.iter().filter_map(|x| x.as_str().map(String::from)).collect::<Vec<_>>()).unwrap_or_default();
    let name = meta.as_ref().and_then(|m| m["name"].as_str()).filter(|s| !s.is_empty()).unwrap_or("loaded").to_string();
    let darwin = meta.as_ref().and_then(|m| m["os"].as_str()) == Some("darwin");
    let target = std::path::PathBuf::from(format!("{}/{}", a.images_dir, name.replace(['/', ':'], "_")));
    let _ = std::fs::remove_dir_all(&target);
    if let Err(e) = std::fs::rename(&staging, &target) { let _ = std::fs::remove_dir_all(&staging); return load_err(e.to_string()); }
    let rootfs = target.join("rootfs");
    let arch = if darwin { Guest::DarwinAarch64 } else { detect_arch(&rootfs).unwrap_or(Guest::LinuxAarch64) };
    let mut cmd = strs("cmd");
    if cmd.is_empty() { cmd = if darwin { vec!["bash".into()] } else { default_shell(&rootfs) }; }
    let (env, entrypoint) = (strs("env"), strs("entrypoint"));
    let workdir = meta.as_ref().and_then(|m| m["workdir"].as_str()).unwrap_or("").to_string();
    let img = Image {
        name: name.clone(), rootfs: rootfs.to_string_lossy().into_owned(), arch,
        cmd: cmd.clone(), env: env.clone(), entrypoint: entrypoint.clone(), workdir: workdir.clone(),
        created: now_secs(), ..Default::default()
    };
    // Persist a dd-image.json so the image round-trips through `discover_images` after a daemon restart.
    let mut dd = json!({ "name": name, "cmd": cmd, "env": env, "entrypoint": entrypoint, "workdir": workdir });
    if darwin { dd["os"] = json!("darwin"); }
    let _ = std::fs::write(target.join("dd-image.json"), dd.to_string());
    register_image(&a, img).await;
    crate::events::emit_event(&a.events, "image", "load", &name, json!({"name": name}));
    Json(json!({ "stream": format!("Loaded image: {}", repo_tag(&name)) })).into_response()
}

/// `docker import` -- extract a bare rootfs tar (request body) into a new image named by `repo`
/// (optionally `repo:tag`) and register it. Routed from `images_create` on `fromSrc`.
pub(crate) async fn image_import(a: App, repo: String, tag: String, src: &str, body: axum::body::Bytes) -> Response {
    if repo.is_empty() { return import_progress(Err("repo is required".into())); }
    // dd imports a rootfs tar streamed in the body (`docker import - <name>`); importing from a remote
    // URL is not supported (dd has no HTTP fetcher).
    if src != "-" { return import_progress(Err(format!("unsupported import source {src:?}; pipe the rootfs to `-`"))); }
    let name = if tag.is_empty() { repo } else { format!("{repo}:{tag}") };
    let target = std::path::PathBuf::from(format!("{}/{}", a.images_dir, name.replace(['/', ':'], "_")));
    let rootfs = target.join("rootfs");
    let _ = std::fs::remove_dir_all(&target);
    if let Err(e) = std::fs::create_dir_all(&rootfs) { return import_progress(Err(e.to_string())); }
    let tmp = std::env::temp_dir().join(format!("dd-import-{}.tar", std::process::id()));
    if let Err(e) = std::fs::write(&tmp, &body) { return import_progress(Err(e.to_string())); }
    let out = std::process::Command::new("tar").arg("xf").arg(&tmp).arg("-C").arg(&rootfs).output();
    let _ = std::fs::remove_file(&tmp);
    match out {
        Ok(o) if o.status.success() => {}
        Ok(o) => return import_progress(Err(String::from_utf8_lossy(&o.stderr).into_owned())),
        Err(e) => return import_progress(Err(e.to_string())),
    }
    let arch = detect_arch(&rootfs).unwrap_or(Guest::LinuxAarch64);
    let cmd = default_shell(&rootfs);
    let img = Image { name: name.clone(), rootfs: rootfs.to_string_lossy().into_owned(), arch, cmd: cmd.clone(), created: now_secs(), ..Default::default() };
    let _ = std::fs::write(target.join("dd-image.json"), json!({ "name": name, "cmd": cmd }).to_string());
    register_image(&a, img).await;
    import_progress(Ok(format!("sha256:{}", fake_id(&name))))
}

/// Re-scan the writable images dir from disk and merge any images not already in the in-memory store
/// (keyed by `repository:tag`). A safety net for a lookup miss: an image whose rootfs + `dd-image.json`
/// exist on disk but isn't registered in memory (e.g. pulled/built by another daemon process, or dropped
/// in out-of-band) becomes visible without a daemon restart. Returns true if anything new was added.
pub(crate) async fn rescan_images(a: &App) -> bool {
    let dir = a.images_dir.clone();
    let found = tokio::task::spawn_blocking(move || discover_images(&dir)).await.unwrap_or_default();
    let mut g = a.inner.lock().await;
    let mut added = false;
    for img in found {
        let tag = repo_tag(&img.name);
        if !g.images.iter().any(|i| repo_tag(&i.name) == tag) {
            g.images.push(img);
            added = true;
        }
    }
    added
}

/// Register a freshly load/import-ed image in the daemon's in-memory state, replacing any existing
/// image sharing the same `repository:tag` (mirrors the re-pull dedupe in `images_create`).
async fn register_image(a: &App, img: Image) {
    let mut g = a.inner.lock().await;
    let tag = repo_tag(&img.name);
    g.images.retain(|i| repo_tag(&i.name) != tag);
    g.images.push(img);
}

/// `docker import` progress: a single JSON status line carrying the new image id, or an error line.
fn import_progress(result: Result<String, String>) -> Response {
    let body = match result {
        Ok(id) => json!({ "status": id }).to_string() + "\r\n",
        Err(e) => json!({ "errorDetail": { "message": e.clone() }, "error": e }).to_string() + "\r\n",
    };
    (StatusCode::OK, [("Content-Type", "application/json")], body).into_response()
}

/// `docker load` failure -> 500 + a Docker-shaped `{"message": …}` error body.
fn load_err(msg: String) -> Response {
    (StatusCode::INTERNAL_SERVER_ERROR, Json(json!({"message": msg}))).into_response()
}

#![allow(unused_imports, dead_code)]
use crate::model::*;
use crate::util::*;
use crate::system::*;
use crate::images::*;
use crate::containers::*;
use crate::build::*;
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


// ===================== docker cp — the /archive tar endpoints =====================
// `docker cp` tars a path out of (GET) / into (PUT) the container filesystem; HEAD returns a path-stat the
// CLI uses to pick file-vs-dir semantics. We operate on the container's rootfs (the overlay upper) -- the
// common case -- except paths under a bind, which `archive_host_path` redirects to the host bind source.
#[derive(serde::Deserialize)]
pub(crate) struct ArchiveQ { path: String }


/// Map a container path to its host path. A path inside a bind volume maps to the host volume dir (so
/// `docker cp` to e.g. ddcli's mounted cwd hits the real files); otherwise it lands in the container
/// rootfs (the overlay upper). `..` is lexically clamped inside whichever base so it can't escape.
pub(crate) fn archive_host_path(rootfs: &str, binds: &[String], path: &str) -> std::path::PathBuf {
    // bind volumes first (host:container with an absolute host source), same precedence as the JIT jail.
    // The most specific (longest container-dest) bind wins so nested binds resolve to the right source;
    // a requested path under a bind hits the HOST source, and only otherwise do we fall back to the rootfs.
    let mut best: Option<(&str, &str)> = None; // (host source, container dest)
    for b in binds {
        let Some((host, cont)) = b.split_once(':') else { continue };
        if !host.starts_with('/') { continue; } // named volumes carry no absolute host source here
        let cont = cont.trim_end_matches('/');
        if path == cont || path.strip_prefix(cont).is_some_and(|r| r.starts_with('/')) {
            if best.map_or(true, |(_, bc)| cont.len() > bc.len()) {
                best = Some((host, cont));
            }
        }
    }
    if let Some((host, cont)) = best {
        return clamp_join(host, &path[cont.len()..]);
    }
    clamp_join(rootfs, path)
}


/// Join `rel` onto `base`, dropping `.`/`..` so the result stays within `base`.
pub(crate) fn clamp_join(base: &str, rel: &str) -> std::path::PathBuf {
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
pub(crate) fn go_filemode(md: &std::fs::Metadata) -> u32 {
    use std::os::unix::fs::PermissionsExt;
    let mut m = md.permissions().mode() & 0o7777;
    let ft = md.file_type();
    if ft.is_dir() { m |= 1 << 31; }
    if ft.is_symlink() { m |= 1 << 27; }
    m
}


/// The `X-Docker-Container-Path-Stat` header value: base64(JSON{name,size,mode,mtime,linkTarget}).
pub(crate) fn path_stat_b64(host: &std::path::Path) -> Option<String> {
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


pub(crate) async fn archive_head(State(a): State<App>, Path(id): Path<String>, Query(q): Query<ArchiveQ>) -> Response {
    let g = a.inner.lock().await;
    let Some(c) = resolve_cid(&g, &id).and_then(|f| g.containers.get(&f)) else { return no_such(&id); };
    match path_stat_b64(&archive_host_path(&c.rootfs, &c.binds, &q.path)) {
        Some(stat) => (StatusCode::OK, [("X-Docker-Container-Path-Stat", stat)]).into_response(),
        None => (StatusCode::NOT_FOUND, Json(json!({"message": format!("Could not find the file {} in container {id}", q.path)}))).into_response(),
    }
}


pub(crate) async fn archive_get(State(a): State<App>, Path(id): Path<String>, Query(q): Query<ArchiveQ>) -> Response {
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


pub(crate) async fn archive_put(State(a): State<App>, Path(id): Path<String>, Query(q): Query<ArchiveQ>, body: axum::body::Bytes) -> Response {
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

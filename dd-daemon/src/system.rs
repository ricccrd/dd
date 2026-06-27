#![allow(unused_imports, dead_code)]
use crate::model::*;
use crate::util::*;
use crate::images::*;
use crate::containers::*;
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

pub(crate) async fn version() -> Json<Value> {
    Json(json!({"Version": "0.1.0-dd", "ApiVersion": API_VERSION, "MinAPIVersion": "1.24",
        "Os": "linux", "Arch": "arm64", "KernelVersion": "6.1.0-dd",
        "GitCommit": "dd00000", "GoVersion": "rustc", "BuildTime": "2024-01-01T00:00:00Z",
        "Experimental": false, "Platform": {"Name": "dd"},
        "Components": [{"Name": "Engine", "Version": "0.1.0-dd",
            "Details": {"ApiVersion": API_VERSION, "Os": "linux", "Arch": "arm64"}}]}))
}

pub(crate) async fn info(State(a): State<App>) -> Json<Value> {
    let g = a.inner.lock().await;
    let running = g.containers.values().filter(|c| c.status == "running").count();
    let paused = g.containers.values().filter(|c| c.status == "paused").count();
    let stopped = g.containers.len() - running - paused;
    Json(json!({"ID": "DD", "Name": "dd", "Containers": g.containers.len(),
        "ContainersRunning": running, "ContainersPaused": paused, "ContainersStopped": stopped,
        "Images": g.images.len(), "Volumes": g.volumes.len(), "Networks": g.networks.len(),
        "Driver": "jit-overlay", "OperatingSystem": "dd (VM-less JIT on macOS)",
        "OSType": "linux", "Architecture": "aarch64", "NCPU": 1, "MemTotal": 0,
        "KernelVersion": "6.1.0-dd", "ServerVersion": "0.1.0-dd",
        "DockerRootDir": dd_home().to_string_lossy(), "CgroupDriver": "none",
        "DefaultRuntime": "dd-jit", "Swarm": {"LocalNodeState": "inactive", "ControlAvailable": false},
        "Plugins": {"Volume": ["local"], "Network": ["bridge", "host", "none"], "Authorization": null, "Log": []},
        "SecurityOptions": [], "Warnings": []}))
}

/// `POST /auth` — `docker login`. dd has no central auth store; accept any credentials so the CLI
/// records them locally (pull/push then send them via `X-Registry-Auth`). Empty body = a probe.
pub(crate) async fn auth(body: axum::body::Bytes) -> Response {
    let _ = body;
    (StatusCode::OK, Json(json!({"Status": "Login Succeeded", "IdentityToken": ""}))).into_response()
}

/// `GET /system/df` — `docker system df`. Reports the rough disk usage of images, containers and
/// volumes. dd has no build cache and no per-container/volume size accounting yet, so `BuildCache`
/// is empty, `BuilderSize` is 0 and the rw/volume sizes take Docker's "not calculated" sentinels.
pub(crate) async fn system_df(State(a): State<App>) -> Json<Value> {
    let g = a.inner.lock().await;
    let images: Vec<Value> = g.images.iter().map(|i| {
        let size = image_size(&i.rootfs, &i.name);
        // Containers backed by this image (by bare ref name) — Docker's `system df` shows this count.
        let containers = g.containers.values().filter(|c| ref_name(&c.image) == ref_name(&i.name)).count();
        json!({"Id": format!("sha256:{}", fake_id(&i.name)), "ParentId": "",
            "RepoTags": [repo_tag(&i.name)], "Created": 0, "Size": size,
            "SharedSize": 0, "VirtualSize": size, "Containers": containers})
    }).collect();
    let layers: i64 = images.iter().filter_map(|i| i["Size"].as_i64()).sum();
    let containers: Vec<Value> = g.containers.values().map(|c| json!({
        "Id": c.id, "Image": c.image, "Command": "", "Created": c.created,
        "SizeRw": 0, "SizeRootFs": 0, "State": c.status, "Status": c.status,
        "Names": [format!("/{}", if c.name.is_empty() { c.id[..12.min(c.id.len())].to_string() } else { c.name.clone() })]})).collect();
    let volumes: Vec<Value> = g.volumes.iter().map(|v| json!({
        "Name": v.name, "Driver": "local", "Mountpoint": v.mountpoint,
        "UsageData": {"Size": -1, "RefCount": -1}})).collect();
    Json(json!({"LayersSize": layers, "Images": images, "Containers": containers,
        "Volumes": volumes, "BuildCache": [], "BuilderSize": 0}))
}

// `GET /events` — `docker events`. The handler now lives in `crate::events` (the lifecycle bus):
// see `events.rs` for the broadcast-backed, newline-delimited JSON stream and `emit_event`.

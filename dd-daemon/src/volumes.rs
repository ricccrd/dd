#![allow(unused_imports, dead_code)]
use crate::model::*;
use crate::util::*;
use crate::system::*;
use crate::images::*;
use crate::containers::*;
use crate::build::*;
use crate::archive::*;
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


// ---- volumes ---------------------------------------------------------------

pub(crate) fn vol_json(v: &Vol) -> Value {
    let driver = if v.driver.is_empty() { "local".to_string() } else { v.driver.clone() };
    json!({"Name": v.name, "Driver": driver, "Mountpoint": v.mountpoint,
        "CreatedAt": fmt_rfc3339(v.created_at), "Scope": "local", "Labels": v.labels, "Options": v.options})
}

pub(crate) async fn volumes_list(State(a): State<App>) -> Json<Value> {
    let g = a.inner.lock().await;
    Json(json!({"Volumes": g.volumes.iter().map(vol_json).collect::<Vec<_>>(), "Warnings": []}))
}

#[derive(Deserialize)]
pub(crate) struct VolumeCreateBody {
    #[serde(rename = "Name")] name: Option<String>,
    #[serde(rename = "Driver")] driver: Option<String>,
    #[serde(rename = "DriverOpts")] driver_opts: Option<HashMap<String, String>>,
    #[serde(rename = "Labels")] labels: Option<HashMap<String, String>>,
}

pub(crate) async fn volumes_create(State(a): State<App>, Json(body): Json<VolumeCreateBody>) -> Response {
    let name = body.name.filter(|n| !n.is_empty()).unwrap_or_else(|| format!("vol_{}", fake_id("v")[..12].to_string()));
    if !name.chars().all(|c| c.is_ascii_alphanumeric() || c == '_' || c == '-' || c == '.') {
        return (StatusCode::BAD_REQUEST, Json(json!({"message": "invalid volume name"}))).into_response();
    }
    let driver = body.driver.filter(|d| !d.is_empty()).unwrap_or_else(|| "local".into());
    let options = body.driver_opts.unwrap_or_default();
    let labels = body.labels.unwrap_or_default();
    let mountpoint = PathBuf::from(&a.volumes_dir).join(&name);
    let _ = std::fs::create_dir_all(&mountpoint);
    let mut g = a.inner.lock().await;
    let v = if let Some(existing) = g.volumes.iter().find(|v| v.name == name).cloned() {
        existing
    } else {
        let v = Vol { name: name.clone(), mountpoint: mountpoint.to_string_lossy().into_owned(), created_at: now_secs(),
            driver, options, labels };
        g.volumes.push(v.clone());
        save_state(&g, &a.state_path);
        crate::events::emit_event(&a.events, "volume", "create", &name, json!({"driver": "local"}));
        v
    };
    (StatusCode::CREATED, Json(vol_json(&v))).into_response()
}

pub(crate) async fn volume_inspect(State(a): State<App>, Path(name): Path<String>) -> Response {
    match a.inner.lock().await.volumes.iter().find(|v| v.name == name) {
        Some(v) => Json(vol_json(v)).into_response(),
        None => (StatusCode::NOT_FOUND, Json(json!({"message": format!("no such volume: {name}")}))).into_response(),
    }
}

pub(crate) async fn volume_delete(State(a): State<App>, Path(name): Path<String>) -> Response {
    let mut g = a.inner.lock().await;
    let mountpoint = g.volumes.iter().find(|v| v.name == name).map(|v| v.mountpoint.clone());
    let in_use = g.containers.values().any(|c| c.binds.iter().any(|b| {
        b.split(':').next().map_or(false, |src| src == name || mountpoint.as_deref() == Some(src))
    }));
    if in_use {
        return (StatusCode::CONFLICT, Json(json!({"message": format!("remove {name}: volume is in use")}))).into_response();
    }
    let before = g.volumes.len();
    g.volumes.retain(|v| v.name != name);
    if g.volumes.len() != before {
        let _ = std::fs::remove_dir_all(PathBuf::from(&a.volumes_dir).join(&name));
        save_state(&g, &a.state_path);
        crate::events::emit_event(&a.events, "volume", "destroy", &name, json!({"driver": "local"}));
        StatusCode::NO_CONTENT.into_response()
    } else {
        (StatusCode::NOT_FOUND, Json(json!({"message": format!("no such volume: {name}")}))).into_response()
    }
}

/// `POST /volumes/prune` — `docker volume prune`. Removes volumes not referenced by any container's
/// binds and reports reclaimed names. (No space accounting yet.)
pub(crate) async fn volumes_prune(State(a): State<App>) -> Json<Value> {
    let mut g = a.inner.lock().await;
    let in_use: std::collections::HashSet<String> = g.containers.values()
        .flat_map(|c| c.binds.iter().filter_map(|b| b.split(':').next().map(str::to_string)))
        .collect();
    let pruned: Vec<String> = g.volumes.iter()
        .filter(|v| !in_use.contains(&v.name) && !in_use.contains(&v.mountpoint))
        .map(|v| v.name.clone()).collect();
    g.volumes.retain(|v| !pruned.contains(&v.name));
    for name in &pruned { let _ = std::fs::remove_dir_all(std::path::Path::new(&a.volumes_dir).join(name)); }
    save_state(&g, &a.state_path);
    Json(json!({"VolumesDeleted": pruned, "SpaceReclaimed": 0}))
}

#![allow(unused_imports, dead_code)]
use crate::model::*;
use crate::util::*;
use crate::system::*;
use crate::images::*;
use crate::containers::*;
use crate::build::*;
use crate::archive::*;
use crate::volumes::*;
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


// ---- networks --------------------------------------------------------------

pub(crate) fn net_json(n: &Net) -> Value {
    let containers: HashMap<String, Value> = n.containers.iter().map(|c| (c.clone(), json!({"Name": &c[..12.min(c.len())]}))).collect();
    json!({"Id": n.id, "Name": n.name, "Driver": n.driver, "Scope": n.scope,
        "Containers": containers, "Created": fmt_rfc3339(n.created), "EnableIPv6": false, "Internal": false,
        "IPAM": {"Driver": "default", "Config": []}})
}

pub(crate) async fn networks_list(State(a): State<App>) -> Json<Value> {
    let g = a.inner.lock().await;
    Json(json!(g.networks.iter().map(net_json).collect::<Vec<_>>()))
}

#[derive(Deserialize)]
pub(crate) struct NetCreateBody { #[serde(rename = "Name")] name: Option<String>, #[serde(rename = "Driver")] driver: Option<String> }

pub(crate) async fn networks_create(State(a): State<App>, Json(body): Json<NetCreateBody>) -> Response {
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

pub(crate) async fn network_inspect(State(a): State<App>, Path(id): Path<String>) -> Response {
    match a.inner.lock().await.networks.iter().find(|n| net_matches(n, &id)) {
        Some(n) => Json(net_json(n)).into_response(),
        None => network_404(&id),
    }
}

pub(crate) async fn network_delete(State(a): State<App>, Path(id): Path<String>) -> Response {
    let mut g = a.inner.lock().await;
    if g.networks.iter().any(|n| net_matches(n, &id) && is_predefined(&n.name)) {
        return (StatusCode::FORBIDDEN, Json(json!({"message": "predefined network cannot be removed"}))).into_response();
    }
    let before = g.networks.len();
    g.networks.retain(|n| !net_matches(n, &id));
    if g.networks.len() != before { save_state(&g, &a.state_path); StatusCode::NO_CONTENT.into_response() } else { network_404(&id) }
}

#[derive(Deserialize)]
pub(crate) struct NetAttachBody { #[serde(rename = "Container")] container: Option<String> }

pub(crate) async fn network_connect(State(a): State<App>, Path(id): Path<String>, Json(b): Json<NetAttachBody>) -> Response {
    let cid = b.container.unwrap_or_default();
    let mut g = a.inner.lock().await;
    let r = match g.networks.iter_mut().find(|n| net_matches(n, &id)) {
        Some(n) => { if !n.containers.contains(&cid) { n.containers.push(cid); } StatusCode::OK.into_response() }
        None => return network_404(&id),
    };
    save_state(&g, &a.state_path);
    r
}

pub(crate) async fn network_disconnect(State(a): State<App>, Path(id): Path<String>, Json(b): Json<NetAttachBody>) -> Response {
    let cid = b.container.unwrap_or_default();
    let mut g = a.inner.lock().await;
    let r = match g.networks.iter_mut().find(|n| net_matches(n, &id)) {
        Some(n) => { n.containers.retain(|c| c != &cid); StatusCode::OK.into_response() }
        None => return network_404(&id),
    };
    save_state(&g, &a.state_path);
    r
}

pub(crate) fn net_matches(n: &Net, id: &str) -> bool {
    n.id == id || n.name == id || n.id.starts_with(id)
}

pub(crate) fn is_predefined(name: &str) -> bool {
    matches!(name, "bridge" | "host" | "none")
}

pub(crate) fn network_404(id: &str) -> Response {
    (StatusCode::NOT_FOUND, Json(json!({"message": format!("no such network: {id}")}))).into_response()
}

pub(crate) fn default_networks() -> Vec<Net> {
    ["bridge", "host", "none"].iter().map(|name| Net {
        id: fake_id(&format!("net-{name}")), name: name.to_string(),
        driver: if *name == "bridge" { "bridge".into() } else { name.to_string() }, created: 0,
        scope: "local".into(), containers: vec![],
    }).collect()
}

/// `POST /networks/prune` — `docker network prune`. Removes user-defined networks with no attached
/// containers (never the predefined bridge/host/none).
pub(crate) async fn networks_prune(State(a): State<App>) -> Json<Value> {
    let mut g = a.inner.lock().await;
    let pruned: Vec<String> = g.networks.iter()
        .filter(|n| !is_predefined(&n.name) && n.containers.is_empty())
        .map(|n| n.name.clone()).collect();
    g.networks.retain(|n| !pruned.contains(&n.name));
    save_state(&g, &a.state_path);
    Json(json!({"NetworksDeleted": pruned}))
}

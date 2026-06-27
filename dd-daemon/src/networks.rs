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

/// CIDR prefix length of a subnet ("172.18.0.0/16" -> 16), defaulting to /16.
fn subnet_prefix(subnet: &str) -> u32 { subnet.split('/').nth(1).and_then(|p| p.parse().ok()).unwrap_or(16) }

/// Deterministic MAC for an IPv4 (Docker convention): `02:42:` + the four address bytes. Cosmetic.
pub(crate) fn ip_mac(ip: &str) -> String {
    let o: Vec<u8> = ip.split('.').map(|p| p.parse().unwrap_or(0)).collect();
    if o.len() == 4 { format!("02:42:{:02x}:{:02x}:{:02x}:{:02x}", o[0], o[1], o[2], o[3]) } else { "02:42:00:00:00:00".into() }
}

/// Pick the next free `/16` from the `172.18.0.0/12` pool, skipping subnets already in use. Returns
/// `(subnet, gateway)`. `bridge` is special-cased to `172.17.0.0/16` by the caller.
pub(crate) fn alloc_subnet(nets: &[Net]) -> (String, String) {
    for o in 18u32..=31 {
        let sub = format!("172.{o}.0.0/16");
        if !nets.iter().any(|n| n.subnet == sub) { return (sub, format!("172.{o}.0.1")); }
    }
    ("172.18.0.0/16".into(), "172.18.0.1".into()) // pool exhausted — degrade rather than fail
}

/// Next free host address in a network's subnet (`.1` reserved for the gateway, hosts start at `.2`).
/// Assumes a `/16` "172.B.0.0" subnet — IPs are handed out as `172.B.0.N`.
pub(crate) fn alloc_ip(net: &Net) -> String {
    let base = net.subnet.split('/').next().unwrap_or("172.18.0.0");
    let p: Vec<&str> = base.split('.').collect();
    let (a, b) = (p.first().copied().unwrap_or("172"), p.get(1).copied().unwrap_or("18"));
    let used: std::collections::HashSet<&str> = net.endpoints.values().map(|e| e.ip.as_str()).collect();
    for k in 2u32..=254 {
        let ip = format!("{a}.{b}.0.{k}");
        if !used.contains(ip.as_str()) { return ip; }
    }
    format!("{a}.{b}.0.2")
}

/// Join container `cid` (reporting as `cname`) to the network named `net_name` in `nets`: lazily
/// allocate the subnet if absent (e.g. for `bridge` from old state), assign a fresh endpoint IP, and
/// add the cid to the membership list. Idempotent — re-joining returns the existing IP. Returns the IP.
pub(crate) fn join_network(nets: &mut [Net], net_name: &str, cid: &str, cname: &str) -> Option<String> {
    let idx = nets.iter().position(|n| n.name == net_name)?;
    if nets[idx].subnet.is_empty() {
        let (sub, gw) = if net_name == "bridge" { ("172.17.0.0/16".into(), "172.17.0.1".into()) } else { alloc_subnet(nets) };
        nets[idx].subnet = sub;
        nets[idx].gateway = gw;
    }
    let n = &mut nets[idx];
    if let Some(e) = n.endpoints.get(cid) { return Some(e.ip.clone()); }
    let ip = alloc_ip(n);
    if !n.containers.iter().any(|c| c == cid) { n.containers.push(cid.to_string()); }
    n.endpoints.insert(cid.to_string(), Endpoint { name: cname.to_string(), ip: ip.clone() });
    Some(ip)
}

/// Drop a container from a network (membership + endpoint IP). Frees the IP for reuse.
pub(crate) fn leave_network(n: &mut Net, cid: &str) {
    n.containers.retain(|c| c != cid);
    n.endpoints.remove(cid);
}

pub(crate) fn net_json(n: &Net) -> Value {
    let prefix = subnet_prefix(&n.subnet);
    let containers: HashMap<String, Value> = n.endpoints.iter()
        .map(|(cid, e)| (cid.clone(), json!({"Name": e.name, "EndpointID": cid, "MacAddress": ip_mac(&e.ip),
            "IPv4Address": format!("{}/{}", e.ip, prefix), "IPv6Address": ""})))
        .collect();
    let config = if n.subnet.is_empty() { json!([]) } else { json!([{"Subnet": n.subnet, "Gateway": n.gateway}]) };
    json!({"Id": n.id, "Name": n.name, "Driver": n.driver, "Scope": n.scope,
        "Containers": containers, "Created": fmt_rfc3339(n.created), "EnableIPv6": false, "Internal": false,
        "IPAM": {"Driver": "default", "Config": config}})
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
    let (subnet, gateway) = alloc_subnet(&g.networks);
    let n = Net { id: fake_id(&format!("net-{name}")), name, driver: body.driver.unwrap_or_else(|| "bridge".into()),
        scope: "local".into(), containers: vec![], created: now_secs(), subnet, gateway, endpoints: HashMap::new() };
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
    let req = b.container.unwrap_or_default();
    let mut g = a.inner.lock().await;
    // Resolve to a full container id + its reported name before mutating networks (avoids borrowing
    // `g.networks` mutably while `g.containers` is borrowed immutably).
    let (cid, cname) = match resolve_cid(&g, &req).and_then(|f| g.containers.get(&f).map(|c| (f.clone(), endpoint_name(c)))) {
        Some(t) => t,
        None => (req.clone(), req.clone()),
    };
    let net_name = match g.networks.iter().find(|n| net_matches(n, &id)) { Some(n) => n.name.clone(), None => return network_404(&id) };
    join_network(&mut g.networks, &net_name, &cid, &cname);
    save_state(&g, &a.state_path);
    StatusCode::OK.into_response()
}

/// The name a container is reported by on a network: its `--name`, or the 12-char short id.
pub(crate) fn endpoint_name(c: &Container) -> String {
    if c.name.is_empty() { c.id[..12.min(c.id.len())].to_string() } else { c.name.clone() }
}

pub(crate) async fn network_disconnect(State(a): State<App>, Path(id): Path<String>, Json(b): Json<NetAttachBody>) -> Response {
    let req = b.container.unwrap_or_default();
    let mut g = a.inner.lock().await;
    let cid = resolve_cid(&g, &req).unwrap_or(req);
    let r = match g.networks.iter_mut().find(|n| net_matches(n, &id)) {
        Some(n) => { leave_network(n, &cid); StatusCode::OK.into_response() }
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
        // bridge is the default network a container without `--network` lands on, so it gets the
        // canonical 172.17.0.0/16; host/none carry no L3 identity.
        subnet: if *name == "bridge" { "172.17.0.0/16".into() } else { String::new() },
        gateway: if *name == "bridge" { "172.17.0.1".into() } else { String::new() },
        endpoints: HashMap::new(),
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

#![allow(dead_code)]
//! The `docker events` lifecycle bus.
//!
//! A single process-wide [`tokio::sync::broadcast`] channel of Docker event JSON values. Lifecycle
//! handlers (`containers.rs`, `images.rs`, `networks.rs`, `volumes.rs`) publish with [`emit_event`];
//! the long-lived `GET /events` stream ([`events`]) subscribes and writes one JSON object per line
//! (newline-delimited, chunked) — the exact shape `docker events` / the Engine API client decode.
//!
//! Mirrors the [`crate::model::Live`] broadcast pattern (one `Sender` kept in shared state, each
//! client gets its own `Receiver` via `subscribe()`). The `Sender` lives even with zero receivers:
//! `send` just returns `Err` (ignored) and later `subscribe()` calls still work.

use crate::model::App;
use axum::body::Body;
use axum::extract::{Query, State};
use axum::http::StatusCode;
use axum::response::{IntoResponse, Response};
use futures_util::stream;
use serde::Deserialize;
use serde_json::{json, Value};
use std::time::{SystemTime, UNIX_EPOCH};
use tokio::sync::broadcast;

/// The shared event bus. A clone of this `Sender` lives in [`App`]; every `/events` client holds a
/// `Receiver` from `subscribe()`. Carries one fully-formed Docker event JSON object per message.
pub(crate) type EventBus = broadcast::Sender<Value>;

/// Create a fresh bus. Capacity is the per-receiver backlog before a slow client lags (and skips the
/// oldest events rather than blocking publishers). Used by `main.rs` to init `App.events`.
pub(crate) fn new_bus() -> EventBus {
    let (tx, _rx) = broadcast::channel(256);
    tx
}

/// Publish one lifecycle event. `type_`/`action` are Docker's event taxonomy (`"container"`/`"start"`,
/// `"image"`/`"pull"`, `"network"`/`"create"`, `"volume"`/`"destroy"`, ...). `id` is the primary
/// object id (container/image/network/volume). `attrs` is a JSON object of `Actor.Attributes`
/// (`{"name":..,"image":..}`); a non-object is treated as empty. Best-effort: a send with no live
/// `/events` clients is silently dropped.
pub(crate) fn emit_event(bus: &EventBus, type_: &str, action: &str, id: &str, attrs: Value) {
    if bus.receiver_count() == 0 {
        return; // no listeners — skip building the value entirely
    }
    let (secs, nanos) = match SystemTime::now().duration_since(UNIX_EPOCH) {
        Ok(d) => (d.as_secs() as i64, d.as_nanos() as i64),
        Err(_) => (0, 0),
    };
    let attributes = match attrs {
        Value::Object(m) => Value::Object(m),
        _ => json!({}),
    };
    // Docker's event document. `Type`/`Action`/`Actor` are the modern fields; `status`/`id`/`from`
    // are the legacy top-level aliases older clients still read for container events.
    let mut ev = json!({
        "Type": type_,
        "Action": action,
        "Actor": { "ID": id, "Attributes": attributes },
        "scope": "local",
        "time": secs,
        "timeNano": nanos,
    });
    if type_ == "container" {
        ev["status"] = json!(action);
        ev["id"] = json!(id);
        if let Some(image) = ev["Actor"]["Attributes"]["image"].as_str() {
            ev["from"] = json!(image);
        }
    }
    let _ = bus.send(ev); // Err == no receivers; fine.
}

#[derive(Deserialize, Default)]
pub(crate) struct EventsQ {
    /// `docker events --filter`, sent as a URL-encoded JSON map (`{"type":["container"],...}`).
    pub(crate) filters: Option<String>,
    /// `since`/`until` are accepted (so the query deserializes) but not applied — dd's stream is live.
    pub(crate) since: Option<String>,
    pub(crate) until: Option<String>,
}

/// The parsed, best-effort subset of `docker events` filters dd honors.
#[derive(Default, Clone)]
struct Filters {
    types: Vec<String>,      // `type=` (container/image/network/volume/...)
    actions: Vec<String>,    // `event=`/`action=` (start/die/...)
    containers: Vec<String>, // `container=` (id or name)
    images: Vec<String>,     // `image=`
}

impl Filters {
    fn parse(raw: &Option<String>) -> Filters {
        let mut f = Filters::default();
        let Some(s) = raw else { return f };
        let Ok(v) = serde_json::from_str::<Value>(s) else { return f };
        f.types = filter_values(&v, "type");
        f.actions = filter_values(&v, "event");
        f.actions.extend(filter_values(&v, "action"));
        f.containers = filter_values(&v, "container");
        f.images = filter_values(&v, "image");
        f
    }

    /// Does this event pass every active filter? (An empty filter list = "match all" for that key.)
    fn matches(&self, ev: &Value) -> bool {
        let typ = ev["Type"].as_str().unwrap_or("");
        let action = ev["Action"].as_str().unwrap_or("");
        let id = ev["Actor"]["ID"].as_str().unwrap_or("");
        let name = ev["Actor"]["Attributes"]["name"].as_str().unwrap_or("");
        let image = ev["Actor"]["Attributes"]["image"].as_str().unwrap_or("");
        if !self.types.is_empty() && !self.types.iter().any(|t| t == typ) {
            return false;
        }
        if !self.actions.is_empty() && !self.actions.iter().any(|a| a == action) {
            return false;
        }
        if !self.containers.is_empty()
            && !self.containers.iter().any(|c| c == id || c == name || id.starts_with(c.as_str()))
        {
            return false;
        }
        if !self.images.is_empty() && !self.images.iter().any(|i| i == image) {
            return false;
        }
        true
    }
}

/// Extract the string values a Docker filter key carries. The wire format is `map[string][]string`
/// (`{"type":["container"]}`); older/CLI encodings use a set-as-object (`{"type":{"container":true}}`).
/// Both are handled; anything else yields an empty list.
fn filter_values(v: &Value, key: &str) -> Vec<String> {
    match &v[key] {
        Value::Array(a) => a.iter().filter_map(|x| x.as_str().map(String::from)).collect(),
        Value::Object(m) => m.keys().cloned().collect(),
        Value::String(s) => vec![s.clone()],
        _ => Vec::new(),
    }
}

/// `GET /events` — `docker events`. Subscribes to the bus and streams matching events as
/// newline-delimited JSON (one object per line, chunked) on a long-lived connection. The stream ends
/// only when the client disconnects or the bus is torn down (daemon shutdown).
pub(crate) async fn events(State(a): State<App>, Query(q): Query<EventsQ>) -> Response {
    let rx = a.events.subscribe();
    let mut filters = Filters::parse(&q.filters);
    // `--filter container=<name|id>` is matched against each event's Actor.ID / name. Some lifecycle
    // events (die/stop/kill) carry no `name` attribute, so a name-only filter would miss them. Resolve
    // every container filter value to its FULL id now (by name or id-prefix) and add it to the match
    // set, so the id-based match catches all of that container's events regardless of the attributes.
    if !filters.containers.is_empty() {
        let g = a.inner.lock().await;
        let resolved: Vec<String> = filters.containers.iter()
            .filter_map(|c| crate::util::resolve_cid(&g, c)).collect();
        for id in resolved { if !filters.containers.contains(&id) { filters.containers.push(id); } }
    }

    // `unfold` drives the broadcast receiver into a byte stream. Returning `Some` yields a line;
    // `continue` skips a filtered-out / lagged event; `None` ends the stream (bus closed).
    let body = stream::unfold((rx, filters), |(mut rx, filters)| async move {
        loop {
            match rx.recv().await {
                Ok(ev) => {
                    if !filters.matches(&ev) {
                        continue;
                    }
                    let mut line = serde_json::to_vec(&ev).unwrap_or_default();
                    line.push(b'\n');
                    return Some((Ok::<Vec<u8>, std::io::Error>(line), (rx, filters)));
                }
                Err(broadcast::error::RecvError::Lagged(_)) => continue,
                Err(broadcast::error::RecvError::Closed) => return None,
            }
        }
    });

    Response::builder()
        .status(StatusCode::OK)
        .header("Content-Type", "application/json")
        .body(Body::from_stream(body))
        .unwrap()
}

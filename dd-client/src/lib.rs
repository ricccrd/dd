//! `ddclient` — a small typed [Docker Engine API v1.43] client that talks to **dd-daemon**
//! over its Unix socket. Both the `dd` CLI and the `dd-app` GUI use this crate so the wire
//! format lives in exactly one place.
//!
//! The client opens a fresh connection per request (the daemon is a local socket; this keeps
//! the code trivial and is plenty fast for list/poll workloads). All methods are `async`.
//!
//! ```no_run
//! # async fn ex() -> Result<(), ddclient::Error> {
//! let c = ddclient::Client::new("/Users/me/.dd/run/docker.sock");
//! if c.ping().await.is_ok() {
//!     for ct in c.list_containers().await? { println!("{} {}", ct.id, ct.image); }
//! }
//! # Ok(()) }
//! ```

use http_body_util::{BodyExt, Full};
use hyper::body::Bytes;
use hyper_util::rt::TokioIo;
use serde::{Deserialize, Serialize};
use std::path::{Path, PathBuf};
use std::time::Duration;

mod models;
pub use models::*;

/// API version prefix the daemon understands (it also accepts un-prefixed paths).
const API: &str = "/v1.43";

/// Errors surfaced by the client. Kept deliberately small and `Display`-friendly so the GUI
/// can show them directly and the CLI can print them.
#[derive(Debug)]
pub enum Error {
    /// The daemon socket could not be reached (daemon down / wrong path).
    Connect(String),
    /// Transport / HTTP-level failure after connecting.
    Http(String),
    /// The daemon answered with a non-2xx status; carries status + body message.
    Status(u16, String),
    /// Response body was not the JSON we expected.
    Decode(String),
}

impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Error::Connect(s) => write!(f, "cannot reach dd-daemon: {s}"),
            Error::Http(s) => write!(f, "http error: {s}"),
            Error::Status(c, m) => write!(f, "daemon returned {c}: {m}"),
            Error::Decode(s) => write!(f, "bad response: {s}"),
        }
    }
}
impl std::error::Error for Error {}

type Result<T> = std::result::Result<T, Error>;

/// A handle to a dd-daemon reachable at `socket`.
#[derive(Clone)]
pub struct Client {
    socket: PathBuf,
    timeout: Duration,
}

impl Client {
    /// Build a client for the daemon listening on `socket`. Does not connect yet.
    pub fn new(socket: impl AsRef<Path>) -> Self {
        Client { socket: socket.as_ref().to_path_buf(), timeout: Duration::from_secs(30) }
    }

    /// Resolve the default socket: `$DDOCKERD_SOCK`, else `~/.dd/run/docker.sock`.
    pub fn default_socket() -> PathBuf {
        if let Ok(s) = std::env::var("DDOCKERD_SOCK") {
            return PathBuf::from(s);
        }
        let home = std::env::var("HOME").unwrap_or_else(|_| ".".into());
        PathBuf::from(home).join(".dd/run/docker.sock")
    }

    /// The socket path this client targets.
    pub fn socket(&self) -> &Path {
        &self.socket
    }

    // ---- typed endpoints ---------------------------------------------------

    /// `GET /_ping` — returns `Ok(())` when the daemon is alive.
    pub async fn ping(&self) -> Result<()> {
        self.raw(hyper::Method::GET, "/_ping", None).await.map(|_| ())
    }

    /// `GET /version`.
    pub async fn version(&self) -> Result<Version> {
        self.get_json("/version").await
    }

    /// `GET /info`.
    pub async fn info(&self) -> Result<Info> {
        self.get_json("/info").await
    }

    /// `GET /images/json`.
    pub async fn list_images(&self) -> Result<Vec<Image>> {
        self.get_json("/images/json").await
    }

    /// `DELETE /images/{name}`.
    pub async fn remove_image(&self, name: &str) -> Result<()> {
        self.raw(hyper::Method::DELETE, &format!("/images/{name}"), None).await.map(|_| ())
    }

    /// `GET /containers/json?all=1` (we always include stopped containers).
    pub async fn list_containers(&self) -> Result<Vec<Container>> {
        self.get_json("/containers/json?all=1").await
    }

    /// `GET /containers/{id}/json` — full inspect.
    pub async fn inspect_container(&self, id: &str) -> Result<serde_json::Value> {
        self.get_json(&format!("/containers/{id}/json")).await
    }

    /// `POST /containers/create` — returns the new container id.
    pub async fn create_container(&self, spec: &CreateContainer) -> Result<String> {
        let v: serde_json::Value =
            self.post_json("/containers/create", Some(to_value(spec)?)).await?;
        Ok(v.get("Id").and_then(|x| x.as_str()).unwrap_or_default().to_string())
    }

    /// `POST /containers/{id}/start`.
    pub async fn start_container(&self, id: &str) -> Result<()> {
        self.raw(hyper::Method::POST, &format!("/containers/{id}/start"), None).await.map(|_| ())
    }

    /// `GET /containers/{id}/logs` — plain stdout+stderr bytes (the Docker multiplexed stream is
    /// demuxed for you).
    pub async fn container_logs(&self, id: &str) -> Result<Vec<u8>> {
        let (_, body) =
            self.raw(hyper::Method::GET, &format!("/containers/{id}/logs?stdout=1&stderr=1"), None)
                .await?;
        Ok(demux_docker_stream(&body))
    }

    /// `DELETE /containers/{id}`.
    pub async fn remove_container(&self, id: &str) -> Result<()> {
        self.raw(hyper::Method::DELETE, &format!("/containers/{id}"), None).await.map(|_| ())
    }

    /// `GET /volumes`.
    pub async fn list_volumes(&self) -> Result<Vec<Volume>> {
        let v: VolumeList = self.get_json("/volumes").await?;
        Ok(v.volumes)
    }

    /// `POST /volumes/create`.
    pub async fn create_volume(&self, name: &str) -> Result<Volume> {
        self.post_json("/volumes/create", Some(serde_json::json!({ "Name": name }))).await
    }

    /// `DELETE /volumes/{name}`.
    pub async fn remove_volume(&self, name: &str) -> Result<()> {
        self.raw(hyper::Method::DELETE, &format!("/volumes/{name}"), None).await.map(|_| ())
    }

    /// `GET /networks`.
    pub async fn list_networks(&self) -> Result<Vec<Network>> {
        self.get_json("/networks").await
    }

    /// `POST /networks/create`.
    pub async fn create_network(&self, name: &str) -> Result<String> {
        let v: serde_json::Value =
            self.post_json("/networks/create", Some(serde_json::json!({ "Name": name }))).await?;
        Ok(v.get("Id").and_then(|x| x.as_str()).unwrap_or_default().to_string())
    }

    /// `DELETE /networks/{id}`.
    pub async fn remove_network(&self, id: &str) -> Result<()> {
        self.raw(hyper::Method::DELETE, &format!("/networks/{id}"), None).await.map(|_| ())
    }

    // ---- plumbing ----------------------------------------------------------

    async fn get_json<T: for<'de> Deserialize<'de>>(&self, path: &str) -> Result<T> {
        let (_, body) = self.raw(hyper::Method::GET, path, None).await?;
        decode(&body)
    }

    async fn post_json<T: for<'de> Deserialize<'de>>(
        &self,
        path: &str,
        body: Option<serde_json::Value>,
    ) -> Result<T> {
        let (_, b) = self.raw(hyper::Method::POST, path, body).await?;
        if b.is_empty() {
            // Some POSTs answer 204 with no body; decode an empty object so `()`/Value works.
            return decode(b"{}");
        }
        decode(&b)
    }

    /// Issue one request and return `(status, body)` for any 2xx; map everything else to [`Error`].
    async fn raw(
        &self,
        method: hyper::Method,
        path: &str,
        body: Option<serde_json::Value>,
    ) -> Result<(hyper::StatusCode, Bytes)> {
        let fut = self.raw_inner(method, path, body);
        match tokio::time::timeout(self.timeout, fut).await {
            Ok(r) => r,
            Err(_) => Err(Error::Http("request timed out".into())),
        }
    }

    async fn raw_inner(
        &self,
        method: hyper::Method,
        path: &str,
        body: Option<serde_json::Value>,
    ) -> Result<(hyper::StatusCode, Bytes)> {
        let stream = tokio::net::UnixStream::connect(&self.socket)
            .await
            .map_err(|e| Error::Connect(format!("{} ({})", e, self.socket.display())))?;
        let io = TokioIo::new(stream);
        let (mut sender, conn) = hyper::client::conn::http1::handshake(io)
            .await
            .map_err(|e| Error::Http(e.to_string()))?;
        // Drive the connection in the background; it ends when the response is done.
        tokio::spawn(async move {
            let _ = conn.await;
        });

        let payload = match &body {
            Some(v) => Full::new(Bytes::from(serde_json::to_vec(v).unwrap_or_default())),
            None => Full::new(Bytes::new()),
        };
        let req = hyper::Request::builder()
            .method(method)
            .uri(format!("{API}{path}"))
            .header(hyper::header::HOST, "dd")
            .header(hyper::header::CONTENT_TYPE, "application/json")
            .body(payload)
            .map_err(|e| Error::Http(e.to_string()))?;

        let resp = sender.send_request(req).await.map_err(|e| Error::Http(e.to_string()))?;
        let status = resp.status();
        let bytes = resp
            .into_body()
            .collect()
            .await
            .map_err(|e| Error::Http(e.to_string()))?
            .to_bytes();

        if status.is_success() {
            Ok((status, bytes))
        } else {
            let msg = serde_json::from_slice::<serde_json::Value>(&bytes)
                .ok()
                .and_then(|v| v.get("message").and_then(|m| m.as_str()).map(String::from))
                .unwrap_or_else(|| String::from_utf8_lossy(&bytes).into_owned());
            Err(Error::Status(status.as_u16(), msg))
        }
    }
}

/// Strip Docker's 8-byte log-frame headers (`[stream,0,0,0,len(4B BE)]`). If the body isn't framed
/// (first byte isn't a 0/1/2 stream marker with zero padding), it's returned unchanged.
fn demux_docker_stream(b: &[u8]) -> Vec<u8> {
    let framed = b.len() >= 8 && b[0] <= 2 && b[1] == 0 && b[2] == 0 && b[3] == 0;
    if !framed {
        return b.to_vec();
    }
    let mut out = Vec::with_capacity(b.len());
    let mut i = 0;
    while i + 8 <= b.len() && b[i] <= 2 && b[i + 1] == 0 && b[i + 2] == 0 && b[i + 3] == 0 {
        let len = u32::from_be_bytes([b[i + 4], b[i + 5], b[i + 6], b[i + 7]]) as usize;
        i += 8;
        let end = (i + len).min(b.len());
        out.extend_from_slice(&b[i..end]);
        i = end;
    }
    out
}

fn decode<T: for<'de> Deserialize<'de>>(b: &[u8]) -> Result<T> {
    serde_json::from_slice(b).map_err(|e| Error::Decode(format!("{e}: {}", String::from_utf8_lossy(b))))
}

fn to_value<T: Serialize>(v: &T) -> Result<serde_json::Value> {
    serde_json::to_value(v).map_err(|e| Error::Decode(e.to_string()))
}

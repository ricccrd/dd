#![allow(unused_imports, dead_code)]
use crate::util::*;
use crate::system::*;
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


#[derive(Clone, Default)]
pub(crate) struct Image {
    pub(crate) name: String,
    pub(crate) rootfs: String,
    pub(crate) arch: Guest,
    pub(crate) cmd: Vec<String>,
    // the rest of the OCI/Dockerfile config metadata a container inherits at run
    pub(crate) env: Vec<String>,        // "K=V" entries (ENV)
    pub(crate) entrypoint: Vec<String>, // ENTRYPOINT (prepended to the command)
    pub(crate) workdir: String,         // WORKDIR / Config.WorkingDir
    pub(crate) created: i64,            // unix secs; image creation/discovery time
    pub(crate) labels: std::collections::HashMap<String, String>, // LABEL + build --label
}


#[derive(Clone, Default, Serialize, Deserialize)]
pub(crate) struct Container {
    pub(crate) id: String,
    pub(crate) image: String,
    pub(crate) rootfs: String,
    pub(crate) cmd: Vec<String>,
    pub(crate) binds: Vec<String>,
    pub(crate) hostname: String,
    pub(crate) memory: i64,
    pub(crate) pids_limit: i64,
    pub(crate) publish: String,
    pub(crate) created: i64,
    pub(crate) status: String,
    pub(crate) exit_code: i64,
    #[serde(default)]
    pub(crate) tty: bool,
    #[serde(default)]
    pub(crate) name: String,
    #[serde(default)]
    pub(crate) working_dir: String,
    #[serde(default)]
    pub(crate) env: Vec<String>, // "K=V" entries from the image ENV + `docker run -e`
    #[serde(default)]
    pub(crate) labels: std::collections::HashMap<String, String>, // `docker run --label`
    #[serde(default)]
    pub(crate) network_mode: String,
    #[serde(default)]
    pub(crate) started_at: i64, // unix secs, set on start (inspect State.StartedAt)
    #[serde(default)]
    pub(crate) finished_at: i64, // unix secs, set on stop/natural exit (inspect State.FinishedAt)
    // Re-derived from the image at load; never serialized.
    #[serde(skip)]
    pub(crate) arch: Option<Guest>,
    // Captured output is not persisted (would bloat the state file).
    #[serde(skip)]
    pub(crate) stdout: Vec<u8>,
    #[serde(skip)]
    pub(crate) stderr: Vec<u8>,
}


/// A running container's live IO plumbing. Created on first attach-or-start, dropped when the guest
/// process exits. The process stdout/stderr fan out to (a) any attached clients via `out`, (b) the log
/// buffers for `docker logs`. `stdin` feeds the guest for `-i`/attach.
pub(crate) struct Live {
    pub(crate) out: broadcast::Sender<(u8, Vec<u8>)>, // (1=stdout, 2=stderr, chunk)
    pub(crate) stdin_tx: mpsc::Sender<Vec<u8>>,        // attach writes here; an empty Vec = stdin EOF
    pub(crate) stdin_rx: Mutex<Option<mpsc::Receiver<Vec<u8>>>>, // start() takes it and feeds the guest
    pub(crate) stdout_buf: Mutex<Vec<u8>>,
    pub(crate) stderr_buf: Mutex<Vec<u8>>,
    pub(crate) exit: watch::Sender<Option<i64>>, // Some(code) once exited
    pub(crate) exit_rx: watch::Receiver<Option<i64>>,
    pub(crate) started: std::sync::atomic::AtomicBool, // start() spawns the process exactly once
    pub(crate) tty: bool,
    pub(crate) pty_master: std::sync::Mutex<Option<RawFd>>, // the PTY master fd (tty containers) for /resize
    pub(crate) pid: std::sync::Mutex<Option<u32>>,          // the live JIT process pid (for pause = SIGSTOP/SIGCONT)
}

impl Live {
    pub(crate) fn new(tty: bool) -> Arc<Self> {
        let (out, _) = broadcast::channel(1024);
        let (exit, exit_rx) = watch::channel(None);
        let (stdin_tx, stdin_rx) = mpsc::channel(256);
        Arc::new(Live { out, stdin_tx, stdin_rx: Mutex::new(Some(stdin_rx)), stdout_buf: Mutex::new(Vec::new()),
            stderr_buf: Mutex::new(Vec::new()), exit, exit_rx,
            started: std::sync::atomic::AtomicBool::new(false), tty,
            pty_master: std::sync::Mutex::new(None), pid: std::sync::Mutex::new(None) })
    }
}


/// A named volume — a directory under the volumes root that containers can bind by name.
#[derive(Clone, Serialize, Deserialize)]
pub(crate) struct Vol {
    pub(crate) name: String,
    pub(crate) mountpoint: String,
    pub(crate) created_at: i64,
    #[serde(default)]
    pub(crate) driver: String, // `docker volume create --driver` (default "local")
    #[serde(default)]
    pub(crate) options: std::collections::HashMap<String, String>, // --opt
    #[serde(default)]
    pub(crate) labels: std::collections::HashMap<String, String>, // --label
}


/// A per-container attachment to a network: the L3 identity (assigned IP) plus the name other
/// containers resolve it by. Keyed by container id in [`Net::endpoints`]. See `docs/design/netstack.md`.
#[derive(Clone, Default, Serialize, Deserialize)]
pub(crate) struct Endpoint {
    pub(crate) name: String, // container name (or short id) — what `network inspect`/embedded DNS reports
    pub(crate) ip: String,   // IPAM-assigned host address within the network subnet, e.g. "172.18.0.2"
}

/// A user-defined network. dd's isolation is a per-container loopback netns (see `run_in_jit`);
/// a network here is metadata plus the set of attached containers and their IPAM identities.
#[derive(Clone, Serialize, Deserialize)]
pub(crate) struct Net {
    pub(crate) id: String,
    pub(crate) name: String,
    pub(crate) driver: String,
    pub(crate) scope: String,
    #[serde(default)]
    pub(crate) containers: Vec<String>,
    #[serde(default)]
    pub(crate) created: i64,
    // IPAM (allocated at create from the 172.18.0.0/12 pool; bridge gets 172.17.0.0/16). Old state
    // files predate these — `#[serde(default)]` keeps them loadable (empty subnet => no IP reported).
    #[serde(default)]
    pub(crate) subnet: String,   // e.g. "172.18.0.0/16"
    #[serde(default)]
    pub(crate) gateway: String,  // e.g. "172.18.0.1" (.1 of the subnet)
    #[serde(default)]
    pub(crate) endpoints: HashMap<String, Endpoint>, // container-id -> assigned endpoint
}


/// A `docker exec` invocation: a command to run in a container's rootfs. dd runs it as a fresh JIT
/// process sharing the container's rootfs + volumes (the same files; a distinct process namespace).
#[derive(Clone)]
pub(crate) struct Exec {
    pub(crate) container_id: String,
    pub(crate) cmd: Vec<String>,
    pub(crate) tty: bool,
    pub(crate) started: bool,
    /// `docker exec -e` overrides (added on top of the container env), `-w` working dir, `-u` user.
    pub(crate) env: Vec<String>,
    pub(crate) working_dir: String,
    pub(crate) user: String,
}


#[derive(Default)]
pub(crate) struct Inner {
    pub(crate) containers: HashMap<String, Container>,
    pub(crate) images: Vec<Image>,
    pub(crate) volumes: Vec<Vol>,
    pub(crate) networks: Vec<Net>,
    pub(crate) live: HashMap<String, Arc<Live>>, // running containers' (and execs') IO plumbing (not persisted)
    pub(crate) execs: HashMap<String, Exec>,     // exec id -> its spec
}


/// The serializable slice of [`Inner`] written to `DD_STATE`.
#[derive(Default, Serialize, Deserialize)]
pub(crate) struct Persisted {
    pub(crate) containers: Vec<Container>,
    pub(crate) volumes: Vec<Vol>,
    pub(crate) networks: Vec<Net>,
}


#[derive(Clone)]
pub(crate) struct App {
    pub(crate) inner: Arc<Mutex<Inner>>,
    pub(crate) state_path: String,
    pub(crate) volumes_dir: String,
    pub(crate) images_dir: String,
    pub(crate) events: crate::events::EventBus, // docker events lifecycle bus
}

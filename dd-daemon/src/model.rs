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
    pub(crate) user: String,            // USER / Config.User — the image's default run user (uid[:gid]/name)
    pub(crate) exposed_ports: Vec<String>, // Config.ExposedPorts keys, e.g. "5432/tcp" (reported by inspect)
    pub(crate) created: i64,            // unix secs; image creation/discovery time
    pub(crate) labels: std::collections::HashMap<String, String>, // LABEL + build --label
}


/// `--restart` policy (HostConfig.RestartPolicy). `name` is one of "" / "no" / "always" /
/// "unless-stopped" / "on-failure"; `max_retry` caps `on-failure` restarts. Serde-renamed so it both
/// deserializes from the create body and round-trips verbatim back through inspect HostConfig.
#[derive(Clone, Default, Serialize, Deserialize)]
pub(crate) struct RestartPolicy {
    #[serde(rename = "Name", default)] pub(crate) name: String,
    #[serde(rename = "MaximumRetryCount", default)] pub(crate) max_retry: i64,
}

/// `--device` mapping (HostConfig.Devices[]). Metadata only — the JIT does not enforce device cgroups —
/// stored and reported verbatim in inspect.
#[derive(Clone, Default, Serialize, Deserialize)]
pub(crate) struct DeviceMapping {
    #[serde(rename = "PathOnHost", default)] pub(crate) path_on_host: String,
    #[serde(rename = "PathInContainer", default)] pub(crate) path_in_container: String,
    #[serde(rename = "CgroupPermissions", default)] pub(crate) cgroup_permissions: String,
}

/// `--mount` spec (HostConfig.Mounts[]). `typ` is "bind" or "volume"; `source` is a host path (bind) or
/// volume name (volume); `target` is the in-container path. `read_only` is metadata (the JIT's Volume
/// mechanism can't mark a mount read-only). Wired into the rootfs via the same path as `-v`/Binds.
#[derive(Clone, Default, Serialize, Deserialize)]
pub(crate) struct Mount {
    #[serde(rename = "Type", default)] pub(crate) typ: String,
    #[serde(rename = "Source", default)] pub(crate) source: String,
    #[serde(rename = "Target", default)] pub(crate) target: String,
    #[serde(rename = "ReadOnly", default)] pub(crate) read_only: bool,
}

#[derive(Clone, Default, Serialize, Deserialize)]
pub(crate) struct Container {
    pub(crate) id: String,
    pub(crate) image: String,
    pub(crate) rootfs: String,
    // Per-container copy-on-write UPPER layer: a private writable dir overlaid on the read-only image
    // `rootfs` (the lower). The guest's writes/creates land here and deletions become whiteouts, so a
    // container never mutates the shared image. Allocated under `<dd_home>/containers/<id>/upper` at
    // create (linux guests only; darwin uses the native jail) and reclaimed on `docker rm`/prune. Empty
    // for darwin containers and for state predating overlay, in which case the flat `rootfs` is used.
    #[serde(default)]
    pub(crate) upper: String,
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
    pub(crate) user: String, // `docker run --user` / `docker exec -u` -> DD_UID/DD_GID
    #[serde(default)]
    pub(crate) started_at: i64, // unix secs, set on start (inspect State.StartedAt)
    #[serde(default)]
    pub(crate) finished_at: i64, // unix secs, set on stop/natural exit (inspect State.FinishedAt)
    // ---- HostConfig fidelity (docker run extras) ----
    #[serde(default)]
    pub(crate) restart_policy: RestartPolicy, // `--restart` (supervisor restarts on exit per policy)
    #[serde(default)]
    pub(crate) restart_count: i64, // restarts performed so far (caps `on-failure:N`; inspect RestartCount)
    #[serde(default)]
    pub(crate) cap_add: Vec<String>, // `--cap-add` — metadata (JIT doesn't enforce Linux capabilities)
    #[serde(default)]
    pub(crate) cap_drop: Vec<String>, // `--cap-drop` — metadata
    #[serde(default)]
    pub(crate) devices: Vec<DeviceMapping>, // `--device` — metadata
    #[serde(default)]
    pub(crate) mounts: Vec<Mount>, // `--mount` (wired into the rootfs alongside `-v`/Binds)
    #[serde(default)]
    pub(crate) privileged: bool, // `--privileged` — metadata; relaxes daemon-side guards (none currently)
    #[serde(default)]
    pub(crate) security_opt: Vec<String>, // `--security-opt` (e.g. "sandbox"/"seccomp=untrusted"); an entry
    // matching sandbox/untrusted opts the container into the JIT's untrusted-guest sentry (see spawn_cfg).
    #[serde(default)]
    pub(crate) auto_remove: bool, // `--rm` (HostConfig.AutoRemove): the daemon removes the container on exit.
    // Mutually exclusive with restart_policy in docker; the reaper drops the container after finalizing exit.
    // Re-derived from the image at load; never serialized.
    #[serde(skip)]
    pub(crate) arch: Option<Guest>,
    // `docker exec` only: overrides the id-derived DD_NETNS loopback key so the exec'd process SHARES the
    // TARGET container's 127.0.0.1 address space instead of getting its own isolated loopback. Set to the
    // parent container's id in exec_start; None for a normal container (which keys off its own id).
    // Runtime-only (lives on the throwaway exec temp), never persisted.
    #[serde(skip)]
    pub(crate) netns_key: Option<String>,
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
    /// Chronological `docker logs` replay record: one entry per output chunk, in arrival order, as
    /// `(emit unix-secs, stream 1=stdout/2=stderr, bytes)`. A single ordered log (replacing the old
    /// per-stream `stdout_buf`/`stderr_buf`) so the buffered replay interleaves stdout/stderr exactly as
    /// the live `out` broadcast does. The reaper derives the per-stream `cc.stdout`/`cc.stderr` from it.
    pub(crate) log_chunks: Mutex<Vec<(i64, u8, Vec<u8>)>>,
    pub(crate) exit: watch::Sender<Option<i64>>, // Some(code) once exited
    pub(crate) exit_rx: watch::Receiver<Option<i64>>,
    /// Fired `true` once NO more output will ever reach `out`: the reaper sets it only AFTER the
    /// stdout/stderr pump tasks have fully drained the guest's pipes/PTY into the broadcast. `exit`
    /// fires the instant the process dies (so `wait`/inspect/logs stay responsive), but at that moment
    /// the pumps -- separate tasks -- may not have broadcast the final bytes yet. A streaming consumer
    /// (attach/exec hijack, `logs -f`) that closed on `exit` alone would race the pumps and drop a
    /// fast-exiting command's last output; closing on `out_done` instead guarantees a complete stream.
    pub(crate) out_done: watch::Sender<bool>,
    pub(crate) out_done_rx: watch::Receiver<bool>,
    pub(crate) started: std::sync::atomic::AtomicBool, // start() spawns the process exactly once
    pub(crate) stop_requested: std::sync::atomic::AtomicBool, // set by stop/kill/rm so the RestartPolicy supervisor won't auto-restart a deliberately-stopped container
    pub(crate) tty: bool,
    pub(crate) pty_master: std::sync::Mutex<Option<RawFd>>, // the PTY master fd (tty containers) for /resize
    pub(crate) pid: std::sync::Mutex<Option<u32>>,          // the live JIT process pid (for pause = SIGSTOP/SIGCONT)
}

impl Live {
    pub(crate) fn new(tty: bool) -> Arc<Self> {
        let (out, _) = broadcast::channel(1024);
        let (exit, exit_rx) = watch::channel(None);
        let (out_done, out_done_rx) = watch::channel(false);
        let (stdin_tx, stdin_rx) = mpsc::channel(256);
        Arc::new(Live { out, stdin_tx, stdin_rx: Mutex::new(Some(stdin_rx)), log_chunks: Mutex::new(Vec::new()),
            exit, exit_rx, out_done, out_done_rx,
            started: std::sync::atomic::AtomicBool::new(false),
            stop_requested: std::sync::atomic::AtomicBool::new(false), tty,
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
    /// `docker exec --privileged` — metadata only (the JIT doesn't enforce Linux caps); accepted
    /// without error and surfaced in exec inspect, mirroring the container-level `privileged`.
    pub(crate) privileged: bool,
    /// The exec process's exit code, recorded by the reaper when it exits. The reaper then drops the
    /// exec's `Live` (its log buffers/channels) to avoid a leak, but keeps THIS record so a post-exit
    /// `docker exec inspect` still reports the real code (the Live is gone, so inspect reads it here).
    pub(crate) exit_code: i64,
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

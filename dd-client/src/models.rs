//! Wire models for the dd-daemon Docker-API surface. These mirror the JSON the daemon emits
//! (see `dd-daemon/src/main.rs`). Fields are lenient (`#[serde(default)]`) so the client keeps
//! working as the daemon grows new fields.

use serde::{Deserialize, Serialize};

/// `GET /version`.
#[derive(Debug, Clone, Default, Deserialize)]
#[serde(default)]
pub struct Version {
    #[serde(rename = "Version")]
    pub version: String,
    #[serde(rename = "ApiVersion")]
    pub api_version: String,
    #[serde(rename = "Os")]
    pub os: String,
    #[serde(rename = "Arch")]
    pub arch: String,
}

/// `GET /info`.
#[derive(Debug, Clone, Default, Deserialize)]
#[serde(default)]
pub struct Info {
    #[serde(rename = "Containers")]
    pub containers: u64,
    #[serde(rename = "Images")]
    pub images: u64,
    #[serde(rename = "Driver")]
    pub driver: String,
    #[serde(rename = "OperatingSystem")]
    pub operating_system: String,
    #[serde(rename = "Architecture")]
    pub architecture: String,
    #[serde(rename = "ServerVersion")]
    pub server_version: String,
}

/// One entry of `GET /images/json`.
#[derive(Debug, Clone, Default, Deserialize)]
#[serde(default)]
pub struct Image {
    #[serde(rename = "Id")]
    pub id: String,
    #[serde(rename = "RepoTags")]
    pub repo_tags: Vec<String>,
    #[serde(rename = "Architecture")]
    pub architecture: String,
    #[serde(rename = "Size")]
    pub size: i64,
}

impl Image {
    /// First repo tag (e.g. `alpine:latest`), or the short id.
    pub fn name(&self) -> String {
        self.repo_tags.first().cloned().unwrap_or_else(|| short(&self.id))
    }
}

/// A published port from `GET /containers/json`.
#[derive(Debug, Clone, Default, Deserialize)]
#[serde(default)]
pub struct Port {
    #[serde(rename = "PrivatePort")]
    pub private_port: u16,
    #[serde(rename = "PublicPort")]
    pub public_port: u16,
    #[serde(rename = "Type")]
    pub typ: String,
}

/// A bind/volume mount of a container (from `Mounts`).
#[derive(Debug, Clone, Default, Deserialize)]
#[serde(default)]
pub struct Mount {
    #[serde(rename = "Source")]
    pub source: String,
    #[serde(rename = "Destination")]
    pub destination: String,
    #[serde(rename = "Type")]
    pub typ: String,
}

/// One entry of `GET /containers/json`.
#[derive(Debug, Clone, Default, Deserialize)]
#[serde(default)]
pub struct Container {
    #[serde(rename = "Id")]
    pub id: String,
    #[serde(rename = "Image")]
    pub image: String,
    #[serde(rename = "Command")]
    pub command: String,
    #[serde(rename = "Names")]
    pub names: Vec<String>,
    #[serde(rename = "State")]
    pub state: String,
    #[serde(rename = "Status")]
    pub status: String,
    #[serde(rename = "Created")]
    pub created: i64,
    #[serde(rename = "Ports")]
    pub ports: Vec<Port>,
    #[serde(rename = "Mounts")]
    pub mounts: Vec<Mount>,
    #[serde(rename = "ExitCode")]
    pub exit_code: i64,
}

impl Container {
    /// Short 12-char id like the docker CLI shows.
    pub fn short_id(&self) -> String {
        short(&self.id)
    }
    /// Display name (first `Names` entry without the leading slash), falling back to short id.
    pub fn name(&self) -> String {
        self.names
            .first()
            .map(|n| n.trim_start_matches('/').to_string())
            .filter(|n| !n.is_empty())
            .unwrap_or_else(|| self.short_id())
    }
    /// True when the daemon considers this container running.
    pub fn running(&self) -> bool {
        self.state.eq_ignore_ascii_case("running")
    }
    /// A short status word for display (falls back to "created").
    pub fn display_status(&self) -> String {
        if self.status.is_empty() { "created".into() } else { self.status.clone() }
    }
    /// Human "80->18080/tcp, …" string.
    pub fn ports_str(&self) -> String {
        self.ports
            .iter()
            .map(|p| {
                let t = if p.typ.is_empty() { "tcp" } else { &p.typ };
                if p.public_port != 0 {
                    format!("{}->{}/{}", p.public_port, p.private_port, t)
                } else {
                    format!("{}/{}", p.private_port, t)
                }
            })
            .collect::<Vec<_>>()
            .join(", ")
    }
}

/// `GET /volumes` envelope (Docker wraps the list in `{ "Volumes": [...] }`).
#[derive(Debug, Clone, Default, Deserialize)]
#[serde(default)]
pub struct VolumeList {
    #[serde(rename = "Volumes")]
    pub volumes: Vec<Volume>,
}

/// One volume.
#[derive(Debug, Clone, Default, Deserialize)]
#[serde(default)]
pub struct Volume {
    #[serde(rename = "Name")]
    pub name: String,
    #[serde(rename = "Driver")]
    pub driver: String,
    #[serde(rename = "Mountpoint")]
    pub mountpoint: String,
    #[serde(rename = "CreatedAt")]
    pub created_at: String,
}

/// One network from `GET /networks`.
#[derive(Debug, Clone, Default, Deserialize)]
#[serde(default)]
pub struct Network {
    #[serde(rename = "Id")]
    pub id: String,
    #[serde(rename = "Name")]
    pub name: String,
    #[serde(rename = "Driver")]
    pub driver: String,
    #[serde(rename = "Scope")]
    pub scope: String,
    #[serde(rename = "Containers", default)]
    pub containers: std::collections::HashMap<String, serde_json::Value>,
}

impl Network {
    /// Short 12-char id.
    pub fn short_id(&self) -> String {
        short(&self.id)
    }
}

/// Body for `POST /containers/create`, built by the CLI/GUI "run" flow.
#[derive(Debug, Clone, Default, Serialize)]
pub struct CreateContainer {
    #[serde(rename = "Image")]
    pub image: String,
    #[serde(rename = "Cmd", skip_serializing_if = "Vec::is_empty")]
    pub cmd: Vec<String>,
    #[serde(rename = "Hostname", skip_serializing_if = "String::is_empty")]
    pub hostname: String,
    #[serde(rename = "HostConfig")]
    pub host_config: HostConfig,
}

/// Subset of Docker's HostConfig the daemon honours.
#[derive(Debug, Clone, Default, Serialize)]
pub struct HostConfig {
    #[serde(rename = "Binds", skip_serializing_if = "Vec::is_empty")]
    pub binds: Vec<String>,
    #[serde(rename = "Memory", skip_serializing_if = "is_zero")]
    pub memory: i64,
    #[serde(rename = "PidsLimit", skip_serializing_if = "is_zero")]
    pub pids_limit: i64,
    #[serde(rename = "PortBindings", skip_serializing_if = "std::collections::HashMap::is_empty")]
    pub port_bindings: std::collections::HashMap<String, Vec<PortBinding>>,
}

#[derive(Debug, Clone, Default, Serialize)]
pub struct PortBinding {
    #[serde(rename = "HostPort")]
    pub host_port: String,
}

fn is_zero(n: &i64) -> bool {
    *n == 0
}

/// docker-style short id (first 12 hex chars).
pub fn short(id: &str) -> String {
    id.trim_start_matches("sha256:").chars().take(12).collect()
}

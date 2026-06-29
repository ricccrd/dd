//! View models for the dd daemon, built from [`bollard`]'s Docker-API responses. These are the
//! shapes the dd GUI and CLI render; each has a `From<bollard::...>` conversion and the small
//! display helpers (`short_id`, `name`, `ports_str`, …) the UI relies on.

use bollard::models::{ContainerSummary, ImageSummary};

/// One entry of `GET /containers/json`.
#[derive(Debug, Clone, Default)]
pub struct Container {
    pub id: String,
    pub image: String,
    pub command: String,
    pub names: Vec<String>,
    pub state: String,
    pub status: String,
    pub created: i64,
    pub ports: Vec<Port>,
    pub mounts: Vec<Mount>,
    pub exit_code: i64,
}

/// A published port from `GET /containers/json`.
#[derive(Debug, Clone, Default)]
pub struct Port {
    pub private_port: u16,
    pub public_port: u16,
    pub typ: String,
}

/// A bind/volume mount of a container (from `Mounts`).
#[derive(Debug, Clone, Default)]
pub struct Mount {
    pub source: String,
    pub destination: String,
    pub typ: String,
}

impl From<ContainerSummary> for Container {
    fn from(c: ContainerSummary) -> Self {
        Container {
            id: c.id.unwrap_or_default(),
            image: c.image.unwrap_or_default(),
            command: c.command.unwrap_or_default(),
            names: c.names.unwrap_or_default(),
            state: c.state.map(|s| s.to_string()).unwrap_or_default(),
            status: c.status.unwrap_or_default(),
            created: c.created.unwrap_or_default(),
            ports: c
                .ports
                .unwrap_or_default()
                .into_iter()
                .map(|p| Port {
                    private_port: p.private_port,
                    public_port: p.public_port.unwrap_or_default(),
                    typ: p.typ.map(|t| t.to_string()).unwrap_or_default(),
                })
                .collect(),
            mounts: c
                .mounts
                .unwrap_or_default()
                .into_iter()
                .map(|m| Mount {
                    source: m.source.unwrap_or_default(),
                    destination: m.destination.unwrap_or_default(),
                    typ: m.typ.map(|t| t.to_string()).unwrap_or_default(),
                })
                .collect(),
            // bollard's ContainerSummary carries no ExitCode; surfaced via inspect if needed.
            exit_code: 0,
        }
    }
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
    /// True when the container is paused (SIGSTOP'd).
    pub fn paused(&self) -> bool {
        self.state.eq_ignore_ascii_case("paused")
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

/// One entry of `GET /images/json`.
#[derive(Debug, Clone, Default)]
pub struct Image {
    pub id: String,
    pub repo_tags: Vec<String>,
    pub architecture: String,
    pub size: i64,
    /// Unix creation time (seconds) — for newest-first sorting.
    pub created: i64,
}

impl From<ImageSummary> for Image {
    fn from(i: ImageSummary) -> Self {
        // bollard's ImageSummary has no Architecture field (the dd daemon emits it as an extra,
        // which bollard drops). The UI only displays it, so leave it blank.
        Image { id: i.id, repo_tags: i.repo_tags, architecture: String::new(), size: i.size, created: i.created }
    }
}

impl Image {
    /// First repo tag (e.g. `alpine:latest`), or the short id.
    pub fn name(&self) -> String {
        self.repo_tags.first().cloned().unwrap_or_else(|| short(&self.id))
    }
}

/// One volume.
#[derive(Debug, Clone, Default)]
pub struct Volume {
    pub name: String,
    pub driver: String,
    pub mountpoint: String,
    pub scope: String,
    pub labels: Vec<(String, String)>,
    pub options: Vec<(String, String)>,
    /// ISO-8601 creation time (sorts chronologically as a string) — for newest-first sorting.
    pub created_at: String,
}

impl From<bollard::models::Volume> for Volume {
    fn from(v: bollard::models::Volume) -> Self {
        Volume {
            name: v.name,
            driver: v.driver,
            mountpoint: v.mountpoint,
            scope: v.scope.map(|s| s.to_string()).unwrap_or_default(),
            labels: sorted_pairs(v.labels),
            options: sorted_pairs(v.options),
            created_at: v.created_at.unwrap_or_default(),
        }
    }
}

/// One network from `GET /networks`.
#[derive(Debug, Clone, Default)]
pub struct Network {
    pub id: String,
    pub name: String,
    pub driver: String,
    pub scope: String,
    pub subnet: String,
    pub gateway: String,
    pub internal: bool,
    pub attachable: bool,
    pub ipv6: bool,
    pub labels: Vec<(String, String)>,
    pub options: Vec<(String, String)>,
    /// IDs of the containers attached to this network (from the inspect `Containers` map).
    pub containers: Vec<String>,
    /// ISO-8601 creation time (sorts chronologically as a string) — for newest-first sorting.
    pub created_at: String,
}

impl From<bollard::models::Network> for Network {
    fn from(n: bollard::models::Network) -> Self {
        let (subnet, gateway) = n
            .ipam
            .and_then(|i| i.config)
            .and_then(|c| c.into_iter().next())
            .map(|c| (c.subnet.unwrap_or_default(), c.gateway.unwrap_or_default()))
            .unwrap_or_default();
        Network {
            id: n.id.unwrap_or_default(),
            name: n.name.unwrap_or_default(),
            driver: n.driver.unwrap_or_default(),
            scope: n.scope.unwrap_or_default(),
            subnet,
            gateway,
            internal: n.internal.unwrap_or_default(),
            attachable: n.attachable.unwrap_or_default(),
            ipv6: n.enable_ipv6.unwrap_or_default(),
            labels: sorted_pairs(n.labels.unwrap_or_default()),
            options: sorted_pairs(n.options.unwrap_or_default()),
            // bollard's list `Network` model carries no container map (only on inspect).
            containers: Vec::new(),
            created_at: n.created.unwrap_or_default(),
        }
    }
}

/// Sort a `{k: v}` map into stable `(k, v)` display pairs.
fn sorted_pairs(m: std::collections::HashMap<String, String>) -> Vec<(String, String)> {
    let mut v: Vec<(String, String)> = m.into_iter().collect();
    v.sort_by(|a, b| a.0.cmp(&b.0));
    v
}

impl Network {
    /// Short 12-char id.
    pub fn short_id(&self) -> String {
        short(&self.id)
    }
}

/// Body for `create_container`, built by the CLI/GUI "run" flow. Only `image` is set today.
#[derive(Debug, Clone, Default)]
pub struct CreateContainer {
    pub image: String,
}

/// Engine info for the System view (`/version` + `/info`, flattened).
#[derive(Debug, Clone, Default)]
pub struct SystemInfo {
    pub version: String,
    pub api_version: String,
    pub os: String,
    pub arch: String,
    pub kernel: String,
    pub driver: String,
    pub root_dir: String,
    pub server_version: String,
    pub ncpu: i64,
    pub mem_total: i64,
    pub containers: i64,
    pub running: i64,
    pub paused: i64,
    pub stopped: i64,
    pub images: i64,
}

/// Disk usage (`/system/df`).
#[derive(Debug, Clone, Default)]
pub struct DiskUsage {
    pub layers_size: i64,
    pub images: i64,
    pub containers: i64,
    pub volumes: i64,
}

/// docker-style short id (first 12 hex chars).
pub fn short(id: &str) -> String {
    id.trim_start_matches("sha256:").chars().take(12).collect()
}

//! `ddclient` — the shared client both the dd GUI and CLI use to talk to **dd-daemon** over its
//! Unix socket. The wire transport is [`bollard`] (the mature Docker-Engine-API crate); this crate
//! wraps it behind a small façade with dd-specific view models, so the consumers depend on one
//! place and never touch bollard's types directly.
//!
//! ```no_run
//! # async fn ex() -> Result<(), ddclient::Error> {
//! let c = ddclient::Client::new("/Users/me/.dd/run/docker.sock");
//! if c.ping().await.is_ok() {
//!     for ct in c.list_containers().await? { println!("{} {}", ct.id, ct.image); }
//! }
//! # Ok(()) }
//! ```

use std::path::{Path, PathBuf};

use bollard::container::LogOutput;
use bollard::models::{ContainerCreateBody, NetworkCreateRequest, VolumeCreateRequest};
use bollard::query_parameters::{
    ListContainersOptionsBuilder, ListImagesOptionsBuilder, ListNetworksOptions, ListVolumesOptions,
    LogsOptionsBuilder, RemoveContainerOptionsBuilder, RemoveImageOptions, RemoveVolumeOptions,
    StartContainerOptions,
};
use bollard::{ClientVersion, Docker};
use futures_util::StreamExt;

mod models;
pub use models::*;

/// Errors surfaced by the client — bollard's, which is `Display` (the GUI shows it, the CLI prints
/// it). Re-exported as `Error` so consumers don't name bollard directly.
pub type Error = bollard::errors::Error;
type Result<T> = std::result::Result<T, Error>;

/// dd-daemon speaks Docker API v1.43.
const VERSION: ClientVersion = ClientVersion { major_version: 1, minor_version: 43 };

/// A handle to a dd-daemon reachable at `socket`. Cheap to clone; connects lazily per request
/// (bollard's connector does no I/O until a call is made), matching the old per-request model.
#[derive(Clone)]
pub struct Client {
    socket: PathBuf,
}

impl Client {
    /// Build a client for the daemon listening on `socket`. Does not connect yet.
    pub fn new(socket: impl AsRef<Path>) -> Self {
        Client { socket: socket.as_ref().to_path_buf() }
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

    fn docker(&self) -> Result<Docker> {
        Docker::connect_with_unix(&self.socket.to_string_lossy(), 30, &VERSION)
    }

    // ---- typed endpoints ---------------------------------------------------

    /// `GET /_ping` — returns `Ok(())` when the daemon is alive.
    pub async fn ping(&self) -> Result<()> {
        self.docker()?.ping().await.map(|_| ())
    }

    /// `GET /containers/json?all=1` (we always include stopped containers).
    pub async fn list_containers(&self) -> Result<Vec<Container>> {
        let opts = ListContainersOptionsBuilder::new().all(true).build();
        let cs = self.docker()?.list_containers(Some(opts)).await?;
        Ok(cs.into_iter().map(Container::from).collect())
    }

    /// `GET /images/json`.
    pub async fn list_images(&self) -> Result<Vec<Image>> {
        let opts = ListImagesOptionsBuilder::new().build();
        let imgs = self.docker()?.list_images(Some(opts)).await?;
        Ok(imgs.into_iter().map(Image::from).collect())
    }

    /// `DELETE /images/{name}`.
    pub async fn remove_image(&self, name: &str) -> Result<()> {
        self.docker()?.remove_image(name, None::<RemoveImageOptions>, None).await.map(|_| ())
    }

    /// `GET /volumes`.
    pub async fn list_volumes(&self) -> Result<Vec<Volume>> {
        let resp = self.docker()?.list_volumes(None::<ListVolumesOptions>).await?;
        Ok(resp.volumes.unwrap_or_default().into_iter().map(Volume::from).collect())
    }

    /// `DELETE /volumes/{name}`.
    pub async fn remove_volume(&self, name: &str) -> Result<()> {
        self.docker()?.remove_volume(name, None::<RemoveVolumeOptions>).await
    }

    /// `POST /volumes/create`.
    pub async fn create_volume(&self, name: &str) -> Result<()> {
        let body = VolumeCreateRequest { name: Some(name.to_string()), ..Default::default() };
        self.docker()?.create_volume(body).await.map(|_| ())
    }

    /// `GET /networks`.
    pub async fn list_networks(&self) -> Result<Vec<Network>> {
        let ns = self.docker()?.list_networks(None::<ListNetworksOptions>).await?;
        Ok(ns.into_iter().map(Network::from).collect())
    }

    /// `POST /networks/create` — returns the new network id.
    pub async fn create_network(&self, name: &str) -> Result<String> {
        let body = NetworkCreateRequest { name: name.to_string(), ..Default::default() };
        Ok(self.docker()?.create_network(body).await?.id)
    }

    /// `DELETE /networks/{id}`.
    pub async fn remove_network(&self, id: &str) -> Result<()> {
        self.docker()?.remove_network(id).await
    }

    /// `POST /containers/create` — returns the new container id.
    pub async fn create_container(&self, spec: &CreateContainer) -> Result<String> {
        let body = ContainerCreateBody { image: Some(spec.image.clone()), ..Default::default() };
        Ok(self.docker()?.create_container(None, body).await?.id)
    }

    /// `POST /containers/{id}/start`.
    pub async fn start_container(&self, id: &str) -> Result<()> {
        self.docker()?.start_container(id, None::<StartContainerOptions>).await
    }

    /// `POST /containers/{id}/stop`.
    pub async fn stop_container(&self, id: &str) -> Result<()> {
        self.docker()?.stop_container(id, None::<bollard::query_parameters::StopContainerOptions>).await
    }

    /// `POST /containers/{id}/restart`.
    pub async fn restart_container(&self, id: &str) -> Result<()> {
        self.docker()?.restart_container(id, None::<bollard::query_parameters::RestartContainerOptions>).await
    }

    /// `POST /containers/{id}/pause`.
    pub async fn pause_container(&self, id: &str) -> Result<()> {
        self.docker()?.pause_container(id).await
    }

    /// `POST /containers/{id}/unpause`.
    pub async fn unpause_container(&self, id: &str) -> Result<()> {
        self.docker()?.unpause_container(id).await
    }

    /// `DELETE /containers/{id}` (force, so running containers are removed too).
    pub async fn remove_container(&self, id: &str) -> Result<()> {
        let opts = RemoveContainerOptionsBuilder::new().force(true).build();
        self.docker()?.remove_container(id, Some(opts)).await
    }

    /// `GET /version` + `GET /info` flattened into one [`SystemInfo`] for the System view.
    pub async fn system(&self) -> Result<SystemInfo> {
        let d = self.docker()?;
        let v = d.version().await.unwrap_or_default();
        let i = d.info().await?;
        Ok(SystemInfo {
            version: v.version.unwrap_or_default(),
            api_version: v.api_version.unwrap_or_default(),
            os: v.os.unwrap_or_default(),
            arch: v.arch.unwrap_or_default(),
            kernel: i.kernel_version.unwrap_or_default(),
            driver: i.driver.unwrap_or_default(),
            root_dir: i.docker_root_dir.unwrap_or_default(),
            server_version: i.server_version.unwrap_or_default(),
            ncpu: i.ncpu.unwrap_or_default(),
            mem_total: i.mem_total.unwrap_or_default(),
            containers: i.containers.unwrap_or_default(),
            running: i.containers_running.unwrap_or_default(),
            paused: i.containers_paused.unwrap_or_default(),
            stopped: i.containers_stopped.unwrap_or_default(),
            images: i.images.unwrap_or_default(),
        })
    }

    /// `GET /system/df` summarized into a [`DiskUsage`].
    pub async fn disk_usage(&self) -> Result<DiskUsage> {
        let r = self.docker()?.df(None::<bollard::query_parameters::DataUsageOptions>).await?;
        Ok(DiskUsage {
            layers_size: r.image_usage.as_ref().and_then(|u| u.total_size).unwrap_or_default(),
            images: r.image_usage.as_ref().and_then(|u| u.total_count).unwrap_or_default(),
            containers: r.container_usage.as_ref().and_then(|u| u.total_count).unwrap_or_default(),
            volumes: r.volume_usage.as_ref().and_then(|u| u.total_count).unwrap_or_default(),
        })
    }

    /// `GET /containers/{id}/logs` — concatenated stdout+stderr bytes (bollard demuxes the Docker
    /// multiplexed frames into `LogOutput` chunks for us).
    pub async fn container_logs(&self, id: &str) -> Result<Vec<u8>> {
        let opts = LogsOptionsBuilder::new().stdout(true).stderr(true).build();
        let mut stream = self.docker()?.logs(id, Some(opts));
        let mut out = Vec::new();
        while let Some(item) = stream.next().await {
            out.extend_from_slice(&log_bytes(item?));
        }
        Ok(out)
    }
}

fn log_bytes(o: LogOutput) -> bytes::Bytes {
    o.into_bytes()
}

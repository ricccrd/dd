//! `docker context` integration. Primary path uses the `docker` CLI; when it isn't installed we
//! write the `~/.docker/contexts` metadata directly (the dir name is the lowercase SHA-256 of the
//! context name). Either way we point at `unix://~/.dd/run/docker.sock`.

use crate::paths;
use sha2::{Digest, Sha256};
use std::io::Write;
use std::process::Command;

/// Context name surfaced in `docker context ls`.
pub const NAME: &str = "dd";

fn have_docker() -> bool {
    Command::new("docker")
        .arg("--version")
        .stdout(std::process::Stdio::null())
        .stderr(std::process::Stdio::null())
        .status()
        .map(|s| s.success())
        .unwrap_or(false)
}

/// Create (or refresh) the `dd` docker context. Returns a human note about which path was taken.
pub fn create() -> std::io::Result<String> {
    let host = paths::docker_host();
    if have_docker() {
        // Remove a stale context first so create is idempotent.
        let _ = Command::new("docker").args(["context", "rm", "-f", NAME]).output();
        let out = Command::new("docker")
            .args([
                "context",
                "create",
                NAME,
                "--description",
                "dd VM-less runtime",
                "--docker",
                &format!("host={host}"),
            ])
            .output()?;
        if out.status.success() {
            return Ok(format!("docker context '{NAME}' -> {host}"));
        }
        // Fall through to manual metadata on CLI failure.
    }
    write_meta(&host)?;
    Ok(format!("wrote ~/.docker/contexts metadata for '{NAME}' -> {host} (docker CLI not used)"))
}

/// `docker context use dd` (no-op note if docker CLI is absent).
pub fn use_context() -> std::io::Result<String> {
    if have_docker() {
        let out = Command::new("docker").args(["context", "use", NAME]).output()?;
        if out.status.success() {
            return Ok(format!("docker context use {NAME}"));
        }
        return Err(std::io::Error::other(String::from_utf8_lossy(&out.stderr).into_owned()));
    }
    Ok("docker CLI not found; set DOCKER_HOST instead".into())
}

/// Remove the `dd` context (CLI if present, else delete the metadata dir).
pub fn remove() -> std::io::Result<()> {
    if have_docker() {
        let _ = Command::new("docker").args(["context", "rm", "-f", NAME]).output();
    }
    let dir = meta_dir();
    if dir.exists() {
        let _ = std::fs::remove_dir_all(&dir);
    }
    Ok(())
}

/// Where docker stores this context's metadata: `~/.docker/contexts/meta/<sha256(name)>`.
fn meta_dir() -> std::path::PathBuf {
    let id = hex(&Sha256::digest(NAME.as_bytes()));
    paths::home().join(".docker/contexts/meta").join(id)
}

fn write_meta(host: &str) -> std::io::Result<()> {
    let dir = meta_dir();
    std::fs::create_dir_all(&dir)?;
    let meta = serde_json::json!({
        "Name": NAME,
        "Metadata": { "Description": "dd VM-less runtime" },
        "Endpoints": { "docker": { "Host": host, "SkipTLSVerify": false } }
    });
    let mut f = std::fs::File::create(dir.join("meta.json"))?;
    f.write_all(serde_json::to_string_pretty(&meta)?.as_bytes())?;
    Ok(())
}

fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}

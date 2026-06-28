//! Canonical filesystem locations for a dd install. Everything lives under the user's `$HOME`
//! so install/uninstall never needs root. The socket sits at `~/.dd/run/docker.sock` — short and
//! space-free so it stays under the ~104-byte `sun_path` limit (never under "Application Support").

use std::path::PathBuf;

/// The launchd label for the per-user daemon agent.
pub const AGENT_LABEL: &str = "com.dd.daemon";

/// Installed app-bundle location (where `dd install` expects the signed `.app`).
pub const APP_BUNDLE: &str = "/Applications/dd.app";

/// `$HOME`, or `.` as a last resort.
pub fn home() -> PathBuf {
    std::env::var_os("HOME").map(PathBuf::from).unwrap_or_else(|| PathBuf::from("."))
}

/// `~/.dd` — state root (images, volumes, state.json, run/).
pub fn dd_root() -> PathBuf {
    home().join(".dd")
}

/// `~/.dd/run` — runtime dir holding the socket.
pub fn run_dir() -> PathBuf {
    dd_root().join("run")
}

/// `~/.dd/run/docker.sock` — the daemon's listen socket (== `DDOCKERD_SOCK`).
pub fn socket() -> PathBuf {
    run_dir().join("docker.sock")
}

/// `~/.dd/images` — image rootfs dirs (== `DD_IMAGES`).
pub fn images_dir() -> PathBuf {
    dd_root().join("images")
}

/// `~/Library/Logs/dd` — daemon stdout/stderr logs.
pub fn logs_dir() -> PathBuf {
    home().join("Library/Logs/dd")
}

/// `~/Library/LaunchAgents/com.dd.daemon.plist`.
pub fn agent_plist() -> PathBuf {
    home().join("Library/LaunchAgents").join(format!("{AGENT_LABEL}.plist"))
}

/// The `dd-daemon` binary the agent should launch. Order: `$DD_DAEMON_BIN`, the installed app
/// bundle, then a binary sitting next to this `dd` executable (the dev/`cargo` layout).
pub fn daemon_bin() -> PathBuf {
    if let Some(p) = std::env::var_os("DD_DAEMON_BIN") {
        return PathBuf::from(p);
    }
    let bundled = PathBuf::from(APP_BUNDLE).join("Contents/Resources/dd-daemon");
    if bundled.exists() {
        return bundled;
    }
    if let Ok(exe) = std::env::current_exe() {
        if let Some(dir) = exe.parent() {
            let sib = dir.join("dd-daemon");
            if sib.exists() {
                return sib;
            }
        }
    }
    bundled
}

/// `unix://<socket>` — the DOCKER_HOST / docker-context endpoint.
pub fn docker_host() -> String {
    format!("unix://{}", socket().display())
}

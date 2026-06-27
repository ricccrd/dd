//! Per-user LaunchAgent management (no root). Writes `~/Library/LaunchAgents/com.dd.daemon.plist`
//! and drives it with the modern `launchctl bootstrap/bootout/kickstart` API (the per-user GUI
//! domain `gui/<uid>`), which never requires `sudo`.

use crate::paths;
use std::io::Write;
use std::process::Command;

/// The `gui/<uid>` service target launchd uses for per-user agents.
pub fn domain_target() -> String {
    let uid = unsafe { libc_getuid() };
    format!("gui/{uid}")
}

// Avoid a libc dependency for one call.
extern "C" {
    #[link_name = "getuid"]
    fn libc_getuid() -> u32;
}

/// The `gui/<uid>/com.dd.daemon` service name for kickstart/bootout/print.
pub fn service_target() -> String {
    format!("{}/{}", domain_target(), paths::AGENT_LABEL)
}

/// Render the LaunchAgent plist. launchd does **not** expand `~`, so every path is absolute.
pub fn render_plist() -> String {
    let daemon = paths::daemon_bin();
    // The JIT binaries (ddjit-*) live next to the daemon inside the bundle's Resources dir.
    let jit_dir = daemon.parent().map(|p| p.to_path_buf()).unwrap_or_else(|| paths::dd_root());
    let sock = paths::socket();
    let images = paths::images_dir();
    let out = paths::logs_dir().join("daemon.out.log");
    let err = paths::logs_dir().join("daemon.err.log");
    format!(
        r#"<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Label</key>            <string>{label}</string>
  <key>ProgramArguments</key>
  <array>
    <string>{daemon}</string>
  </array>
  <key>EnvironmentVariables</key>
  <dict>
    <key>DDOCKERD_SOCK</key>  <string>{sock}</string>
    <key>DD_IMAGES</key>      <string>{images}</string>
    <key>DDJIT_DIR</key>      <string>{jit_dir}</string>
  </dict>
  <key>RunAtLoad</key>        <true/>
  <key>KeepAlive</key>        <true/>
  <key>ProcessType</key>      <string>Interactive</string>
  <key>StandardOutPath</key>  <string>{out}</string>
  <key>StandardErrorPath</key><string>{err}</string>
</dict>
</plist>
"#,
        label = paths::AGENT_LABEL,
        daemon = daemon.display(),
        sock = sock.display(),
        images = images.display(),
        jit_dir = jit_dir.display(),
        out = out.display(),
        err = err.display(),
    )
}

/// Create the `~/.dd` tree and write the plist (does not load it).
pub fn write_plist() -> std::io::Result<()> {
    for d in [paths::run_dir(), paths::images_dir(), paths::dd_root().join("volumes"), paths::logs_dir()] {
        std::fs::create_dir_all(&d)?;
    }
    let plist = paths::agent_plist();
    if let Some(parent) = plist.parent() {
        std::fs::create_dir_all(parent)?;
    }
    let mut f = std::fs::File::create(&plist)?;
    f.write_all(render_plist().as_bytes())?;
    Ok(())
}

/// `launchctl bootstrap gui/<uid> <plist>` (load + start). Idempotent: a re-bootstrap of an
/// already-loaded agent is treated as success.
pub fn bootstrap() -> std::io::Result<()> {
    // Best-effort bootout first so re-installs pick up a changed plist.
    let _ = bootout();
    let plist = paths::agent_plist();
    run("launchctl", &["bootstrap", &domain_target(), &plist.to_string_lossy()])
}

/// `launchctl bootout gui/<uid>/com.dd.daemon` (stop + unload).
pub fn bootout() -> std::io::Result<()> {
    run("launchctl", &["bootout", &service_target()])
}

/// `launchctl kickstart -k gui/<uid>/com.dd.daemon` (restart).
pub fn kickstart() -> std::io::Result<()> {
    run("launchctl", &["kickstart", "-k", &service_target()])
}

/// `launchctl print gui/<uid>/com.dd.daemon` (status, streamed to our stdout).
pub fn print_status() -> std::io::Result<bool> {
    let st = Command::new("launchctl").args(["print", &service_target()]).status()?;
    Ok(st.success())
}

/// True when the agent is currently loaded.
pub fn is_loaded() -> bool {
    Command::new("launchctl")
        .args(["print", &service_target()])
        .stdout(std::process::Stdio::null())
        .stderr(std::process::Stdio::null())
        .status()
        .map(|s| s.success())
        .unwrap_or(false)
}

fn run(cmd: &str, args: &[&str]) -> std::io::Result<()> {
    let out = Command::new(cmd).args(args).output()?;
    if out.status.success() {
        Ok(())
    } else {
        Err(std::io::Error::other(format!(
            "{cmd} {} failed: {}",
            args.join(" "),
            String::from_utf8_lossy(&out.stderr).trim()
        )))
    }
}

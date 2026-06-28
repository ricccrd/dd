//! Dead-simple self-update: ask GitHub for the latest release and, if it's newer than the running
//! version, download its `.dmg` and swap `/Applications/dd.app`. Uses `curl` + `hdiutil`
//! (always present on macOS), so no HTTP/TLS dependencies.

use serde::Deserialize;
use std::process::Command;

/// The repo to check (owner/name).
const REPO: &str = "huttarichard/dd";

/// A newer release available for install.
#[derive(Debug, Clone)]
pub struct Release {
    pub version: String,
    pub dmg: String,
    pub page: String,
}

#[derive(Deserialize)]
struct Gh {
    tag_name: String,
    html_url: String,
    assets: Vec<Asset>,
    #[serde(default)]
    prerelease: bool,
    #[serde(default)]
    draft: bool,
}
#[derive(Deserialize)]
struct Asset {
    name: String,
    browser_download_url: String,
}

/// The latest release if it's newer than `current` (e.g. `env!("CARGO_PKG_VERSION")`), else `None`.
pub fn check(current: &str) -> Option<Release> {
    let url = format!("https://api.github.com/repos/{REPO}/releases/latest");
    let out = Command::new("curl").args(["-fsSL", "-H", "User-Agent: dd-app", &url]).output().ok()?;
    if !out.status.success() {
        return None;
    }
    let gh: Gh = serde_json::from_slice(&out.stdout).ok()?;
    if gh.draft || gh.prerelease {
        return None;
    }
    let version = gh.tag_name.trim_start_matches('v').to_string();
    if !newer(&version, current) {
        return None;
    }
    let dmg = gh.assets.into_iter().find(|a| a.name.ends_with(".dmg"))?.browser_download_url;
    Some(Release { version, dmg, page: gh.html_url })
}

/// Download the release `.dmg`, replace `/Applications/dd.app`, clear quarantine and reopen.
/// Blocking; the caller should quit after this succeeds (the new copy is already launching).
pub fn install(rel: &Release) -> Result<(), String> {
    let tmp = std::env::temp_dir().join("dd-update");
    let dmg = tmp.join("dd.dmg");
    let mnt = tmp.join("mnt");
    std::fs::create_dir_all(&mnt).map_err(|e| e.to_string())?;
    sh("curl", &["-fsSL", "-o", &dmg.to_string_lossy(), &rel.dmg])?;
    sh("hdiutil", &["attach", &dmg.to_string_lossy(), "-nobrowse", "-mountpoint", &mnt.to_string_lossy()])?;
    let app = std::fs::read_dir(&mnt)
        .ok()
        .and_then(|d| d.flatten().map(|e| e.path()).find(|p| p.extension().is_some_and(|x| x == "app")))
        .ok_or("no .app inside the dmg")?;
    let dest = "/Applications/dd.app";
    let _ = std::fs::remove_dir_all(dest);
    let copied = sh("cp", &["-R", &app.to_string_lossy(), "/Applications/"]);
    let _ = sh("hdiutil", &["detach", &mnt.to_string_lossy(), "-force"]);
    copied?;
    let _ = sh("xattr", &["-dr", "com.apple.quarantine", dest]);
    let _ = Command::new("open").arg(dest).spawn();
    Ok(())
}

fn sh(cmd: &str, args: &[&str]) -> Result<(), String> {
    let ok = Command::new(cmd).args(args).status().map_err(|e| e.to_string())?.success();
    ok.then_some(()).ok_or_else(|| format!("`{cmd}` failed"))
}

/// `a` is strictly newer than `b` (compares the first three numeric components).
fn newer(a: &str, b: &str) -> bool {
    ver(a) > ver(b)
}
fn ver(v: &str) -> (u64, u64, u64) {
    let mut p = v.trim_start_matches('v').split(['.', '-', '+']).filter_map(|x| x.parse().ok());
    (p.next().unwrap_or(0), p.next().unwrap_or(0), p.next().unwrap_or(0))
}

//! `ddcli run` / `ddcli <image>` / `ddcli mac` — launch a container with *easy-access* defaults:
//! the current directory mounted at the same path and used as the working dir, host networking, and an
//! interactive shell when no command is given. We drive the dd daemon through the stock `docker` CLI
//! (pointed at dd's socket), so the streaming/TTY behaviour is exactly docker's.

use std::io::IsTerminal;
use std::process::Command;

use crate::paths;

/// A parsed `run` invocation. `ddcli run …` and the bare-image shorthand `ddcli <image> …` both parse
/// into this via [`parse`].
pub struct RunArgs {
    /// `--platform linux/amd64` etc.; `None` = native (arm64).
    pub platform: Option<String>,
    /// `--isolated`: skip the automatic cwd mount + host networking.
    pub isolated: bool,
    /// `--keep`: don't remove the container when it exits (default is `--rm`).
    pub keep: bool,
    pub image: String,
    /// Command to run instead of the image default (an interactive shell).
    pub command: Vec<String>,
}

/// Parse `[--platform P] [--isolated] [--keep] <image> [command…]`. ddcli's own flags are recognized
/// wherever they appear (before or after the image, matching the casual `ddcli run ubuntu --platform …`);
/// the first remaining token is the image and the rest are the command.
pub fn parse(raw: Vec<String>) -> Result<RunArgs, String> {
    let (mut platform, mut isolated, mut keep) = (None, false, false);
    let mut rest = Vec::new();
    let mut it = raw.into_iter();
    while let Some(a) = it.next() {
        match a.as_str() {
            "--isolated" => isolated = true,
            "--keep" => keep = true,
            "--platform" => platform = it.next(),
            s if s.starts_with("--platform=") => platform = Some(s["--platform=".len()..].to_string()),
            _ => rest.push(a),
        }
    }
    let mut rest = rest.into_iter();
    let image = rest.next().ok_or("usage: ddcli run <image> [command…]")?;
    Ok(RunArgs { platform, isolated, keep, image, command: rest.collect() })
}

/// `ddcli mac` — macOS containers aren't supported yet; explain why rather than failing cryptically.
pub fn mac(_raw: Vec<String>) -> i32 {
    eprintln!("ddcli mac: macOS containers aren't supported yet.");
    eprintln!();
    eprintln!("dd's darwin engine runs only *static, thin, arm64* Mach-O binaries. macOS system tools");
    eprintln!("like /bin/zsh are universal (fat) + arm64e + dynamically linked (they need dyld + libSystem),");
    eprintln!("which the engine doesn't load yet — that needs a full Mach-O dynamic linker.");
    eprintln!();
    eprintln!("Linux containers are the supported path:  ddcli ubuntu   ·   ddcli run <image> …");
    1
}

/// Run a container with the easy-access defaults, by invoking `docker` against dd's socket.
pub fn run(args: RunArgs) -> i32 {
    if !docker_present() {
        eprintln!("ddcli needs the `docker` CLI on PATH — it drives the dd daemon. Install Docker's CLI.");
        return 1;
    }
    if let Err(e) = crate::ensure_daemon() {
        eprintln!("dd daemon isn't reachable: {e}\nTry:  ddcli install");
        return 1;
    }
    let cwd = std::env::current_dir().map(|p| p.to_string_lossy().into_owned()).unwrap_or_else(|_| "/".into());

    let mut cmd = Command::new("docker");
    cmd.arg("--host").arg(paths::docker_host()).arg("run");
    if !args.keep {
        cmd.arg("--rm");
    }
    // Always attach stdin; allocate a TTY only when we actually have one (so pipes still work).
    cmd.arg("-i");
    if std::io::stdin().is_terminal() && std::io::stdout().is_terminal() {
        cmd.arg("-t");
    }
    // Easy access: the current directory is the container's working dir, mounted at the same path, on the
    // host network. `--isolated` opts out for a clean, sandboxed run.
    if !args.isolated {
        cmd.arg("--network").arg("host");
        cmd.arg("-v").arg(format!("{cwd}:{cwd}"));
        cmd.arg("-w").arg(&cwd);
    }
    if let Some(p) = &args.platform {
        cmd.arg("--platform").arg(p);
    }
    cmd.arg(&args.image);
    cmd.args(&args.command);

    match cmd.status() {
        Ok(s) => s.code().unwrap_or(0),
        Err(e) => {
            eprintln!("failed to launch docker: {e}");
            1
        }
    }
}

fn docker_present() -> bool {
    Command::new("docker").arg("--version").output().map(|o| o.status.success()).unwrap_or(false)
}

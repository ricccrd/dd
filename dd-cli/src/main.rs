//! `ddcli` — the user-facing command for the dd VM-less container runtime.
//!
//! Run containers with easy-access defaults (the current directory mounted + the working dir, host
//! networking, an interactive shell), and manage the per-user daemon — all without root.
//!
//!   ddcli ubuntu                       # drop into a shell in an ubuntu container, here in this dir
//!   ddcli run alpine echo hi           # run a one-off command
//!   ddcli run ubuntu --platform linux/amd64   # force amd64 (runs via the x86-64 JIT)
//!   ddcli mac                          # a macOS container (experimental)
//!   ddcli install                      # set up the daemon agent + docker context
//!   ddcli doctor                       # check everything is healthy

mod agent;
mod context;
mod paths;
mod run;

use clap::{Parser, Subcommand};
use std::process::Command;

#[derive(Parser)]
#[command(name = "ddcli", version, about = "ddcli — VM-less containers on macOS")]
struct Cli {
    #[command(subcommand)]
    cmd: Cmd,
}

#[derive(Subcommand)]
enum Cmd {
    /// Run a container: current dir mounted + working dir, host networking, interactive shell.
    ///
    /// Usage: ddcli run [--platform P] [--isolated] [--keep] <image> [command…]
    Run {
        #[arg(trailing_var_arg = true, allow_hyphen_values = true)]
        args: Vec<String>,
    },
    /// Start a macOS container (experimental — the host macOS in a darwin jail).
    Mac {
        #[arg(trailing_var_arg = true, allow_hyphen_values = true)]
        args: Vec<String>,
    },
    /// Launch the dd-app GUI.
    App,
    /// Run or control the background daemon.
    Daemon {
        #[command(subcommand)]
        action: DaemonCmd,
    },
    /// Install the daemon agent + docker context (no root).
    Install,
    /// Remove the daemon agent + docker context.
    Uninstall {
        /// Also delete ~/.dd state (images, volumes, state.json) and logs.
        #[arg(long)]
        purge: bool,
    },
    /// Manage just the docker context.
    Context {
        #[command(subcommand)]
        action: ContextCmd,
    },
    /// Diagnose the install (socket, agent, context, app quarantine).
    Doctor,
    /// `ddcli <image> [command…]` — shorthand for `ddcli run <image> …`.
    #[command(external_subcommand)]
    Image(Vec<String>),
}

#[derive(Subcommand)]
enum DaemonCmd {
    /// Run the daemon in the foreground (what the LaunchAgent execs).
    Run,
    /// Load + start the daemon agent.
    Start,
    /// Stop + unload the daemon agent.
    Stop,
    /// Restart the daemon agent.
    Restart,
    /// Show launchd status for the agent.
    Status,
    /// Tail the daemon logs.
    Logs,
}

#[derive(Subcommand)]
enum ContextCmd {
    /// Create/refresh the `dd` docker context.
    Create,
    /// Remove the `dd` docker context.
    Rm,
    /// `docker context use dd`.
    Use,
    /// Print the context endpoint.
    Show,
}

fn main() {
    let cli = Cli::parse();
    let code = match cli.cmd {
        Cmd::Run { args } => cmd_run(args),
        Cmd::Mac { args } => run::mac(args),
        Cmd::Image(args) => cmd_run(args),
        Cmd::App => cmd_app(),
        Cmd::Daemon { action } => cmd_daemon(action),
        Cmd::Install => cmd_install(),
        Cmd::Uninstall { purge } => cmd_uninstall(purge),
        Cmd::Context { action } => cmd_context(action),
        Cmd::Doctor => cmd_doctor(),
    };
    std::process::exit(code);
}

/// `ddcli run …` and the bare-image shorthand both land here.
fn cmd_run(raw: Vec<String>) -> i32 {
    match run::parse(raw) {
        Ok(args) => run::run(args),
        Err(e) => {
            eprintln!("{e}");
            2
        }
    }
}

/// Ensure the daemon socket answers; if not, try to (re)start the agent and wait briefly for it.
fn ensure_daemon() -> Result<(), String> {
    let sock = paths::socket();
    if ping_socket(&sock) {
        return Ok(());
    }
    let _ = ensure_agent(); // best-effort: load the LaunchAgent (macOS)
    for _ in 0..40 {
        if ping_socket(&sock) {
            return Ok(());
        }
        std::thread::sleep(std::time::Duration::from_millis(100));
    }
    Err(format!("no daemon listening at {}", sock.display()))
}

/// Launch the installed GUI bundle (or a dev `dd-app` sibling binary).
fn cmd_app() -> i32 {
    let bundle = std::path::Path::new(paths::APP_BUNDLE);
    if bundle.exists() {
        // `open` detaches the GUI from this terminal.
        return run_status(Command::new("open").arg(bundle));
    }
    // Dev fallback: a dd-app binary next to us.
    if let Ok(exe) = std::env::current_exe() {
        if let Some(sib) = exe.parent().map(|d| d.join("dd-app")) {
            if sib.exists() {
                return run_status(&mut Command::new(sib));
            }
        }
    }
    eprintln!("dd-app not found. Install it (drag dd-app.app to /Applications) or build with `make app`.");
    1
}

fn cmd_daemon(action: DaemonCmd) -> i32 {
    match action {
        DaemonCmd::Run => daemon_run(),
        DaemonCmd::Start => report("daemon start", ensure_agent()),
        DaemonCmd::Stop => report("daemon stop", agent::bootout()),
        DaemonCmd::Restart => report("daemon restart", agent::kickstart()),
        DaemonCmd::Status => {
            let _ = agent::print_status();
            0
        }
        DaemonCmd::Logs => {
            let log = paths::logs_dir().join("daemon.err.log");
            run_status(Command::new("tail").args(["-n", "200", "-f"]).arg(log))
        }
    }
}

/// Exec the dd-daemon binary in the foreground with the canonical env.
fn daemon_run() -> i32 {
    use std::os::unix::process::CommandExt;
    let _ = std::fs::create_dir_all(paths::run_dir());
    let _ = std::fs::create_dir_all(paths::images_dir());
    let bin = paths::daemon_bin();
    if !bin.exists() {
        eprintln!("dd-daemon binary not found at {}", bin.display());
        return 1;
    }
    let mut cmd = Command::new(&bin);
    cmd.env("DDOCKERD_SOCK", paths::socket()).env("DD_IMAGES", paths::images_dir());
    if let Some(dir) = bin.parent() {
        cmd.env("DDJIT_DIR", dir); // find ddjit-* next to the daemon (bundle Resources)
    }
    let err = cmd.exec(); // only returns on failure
    eprintln!("exec {} failed: {err}", bin.display());
    1
}

/// Full install: state tree + LaunchAgent + docker context, then a health hint.
fn cmd_install() -> i32 {
    if let Err(e) = agent::write_plist() {
        eprintln!("write LaunchAgent: {e}");
        return 1;
    }
    println!("✓ wrote {}", paths::agent_plist().display());

    match ensure_agent() {
        Ok(_) => println!("✓ loaded daemon agent ({})", agent::service_target()),
        Err(e) => eprintln!("! could not load agent: {e}"),
    }
    match context::create() {
        Ok(note) => println!("✓ {note}"),
        Err(e) => eprintln!("! docker context: {e}"),
    }
    let _ = context::use_context().map(|n| println!("✓ {n}"));

    println!("\nIf you don't use `docker context`, add this to your shell:");
    println!("    export DOCKER_HOST={}", paths::docker_host());
    warn_quarantine();
    println!("\nDone. Try:  ddcli ubuntu   (a shell in an ubuntu container, here)  ·  ddcli doctor");
    0
}

fn cmd_uninstall(purge: bool) -> i32 {
    let _ = agent::bootout();
    let _ = std::fs::remove_file(paths::agent_plist());
    println!("✓ removed daemon agent + plist");
    let _ = context::remove();
    println!("✓ removed docker context '{}'", context::NAME);
    if purge {
        let _ = std::fs::remove_dir_all(paths::dd_root());
        let _ = std::fs::remove_dir_all(paths::logs_dir());
        println!("✓ purged {} and logs", paths::dd_root().display());
    }
    0
}

fn cmd_context(action: ContextCmd) -> i32 {
    match action {
        ContextCmd::Create => note("context create", context::create()),
        ContextCmd::Rm => report("context rm", context::remove()),
        ContextCmd::Use => note("context use", context::use_context()),
        ContextCmd::Show => {
            println!("name:     {}", context::NAME);
            println!("endpoint: {}", paths::docker_host());
            0
        }
    }
}

/// Health check: socket reachable, agent loaded, context set, app present/unquarantined.
fn cmd_doctor() -> i32 {
    let mut ok = true;

    let agent_loaded = agent::is_loaded();
    line(agent_loaded, &format!("daemon agent loaded ({})", agent::service_target()));
    ok &= agent_loaded;

    let sock = paths::socket();
    let reachable = ping_socket(&sock);
    line(reachable, &format!("daemon socket reachable ({})", sock.display()));
    ok &= reachable;

    let ctx_dir = paths::home().join(".docker/contexts/meta");
    let ctx = ctx_dir.exists();
    line(ctx, "docker context present (~/.docker/contexts)");

    let bundle = std::path::Path::new(paths::APP_BUNDLE);
    if bundle.exists() {
        line(true, &format!("app installed ({})", paths::APP_BUNDLE));
        let quarantined = is_quarantined(bundle);
        line(!quarantined, "app not gatekeeper-quarantined");
        if quarantined {
            println!("    fix: xattr -dr com.apple.quarantine {}", paths::APP_BUNDLE);
        }
    } else {
        line(false, &format!("app not installed at {}", paths::APP_BUNDLE));
        println!("    install: build with `make dmg`, then drag dd-app.app to /Applications");
    }

    if !ok {
        println!("\nSome checks failed. `ddcli install` sets up the agent + context.");
        println!("If the GUI renders oddly, try:  GSK_RENDERER=cairo open {}", paths::APP_BUNDLE);
    }
    if ok {
        0
    } else {
        1
    }
}

// ---- helpers ---------------------------------------------------------------

/// Write the plist (if missing) and bootstrap the agent.
fn ensure_agent() -> Result<(), String> {
    if !paths::agent_plist().exists() {
        agent::write_plist().map_err(|e| e.to_string())?;
    }
    agent::bootstrap().map_err(|e| e.to_string())
}

/// Synchronously connect to the socket to confirm the daemon answers `_ping`.
fn ping_socket(sock: &std::path::Path) -> bool {
    let rt = match tokio::runtime::Builder::new_current_thread().enable_all().build() {
        Ok(rt) => rt,
        Err(_) => return false,
    };
    rt.block_on(async {
        let c = ddclient::Client::new(sock);
        c.ping().await.is_ok()
    })
}

fn is_quarantined(p: &std::path::Path) -> bool {
    Command::new("xattr")
        .arg("-p")
        .arg("com.apple.quarantine")
        .arg(p)
        .output()
        .map(|o| o.status.success())
        .unwrap_or(false)
}

fn warn_quarantine() {
    let bundle = std::path::Path::new(paths::APP_BUNDLE);
    if bundle.exists() && is_quarantined(bundle) {
        println!("\nThe app is quarantined by Gatekeeper. Clear it once with:");
        println!("    xattr -dr com.apple.quarantine {}", paths::APP_BUNDLE);
    }
}

fn line(ok: bool, msg: &str) {
    println!("[{}] {msg}", if ok { "✓" } else { "✗" });
}

/// Report success/failure of an action whose Ok payload we don't need to show.
fn report<T, E: std::fmt::Display>(what: &str, r: Result<T, E>) -> i32 {
    match r {
        Ok(_) => {
            println!("✓ {what}");
            0
        }
        Err(e) => {
            eprintln!("✗ {what}: {e}");
            1
        }
    }
}

/// Like [`report`] but prints the Ok payload (a human note) too.
fn note<E: std::fmt::Display>(what: &str, r: Result<String, E>) -> i32 {
    match r {
        Ok(n) => {
            println!("✓ {what}: {n}");
            0
        }
        Err(e) => {
            eprintln!("✗ {what}: {e}");
            1
        }
    }
}

fn run_status(cmd: &mut Command) -> i32 {
    match cmd.status() {
        Ok(s) => s.code().unwrap_or(0),
        Err(e) => {
            eprintln!("failed to run: {e}");
            1
        }
    }
}

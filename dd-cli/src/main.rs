//! `dd` — the user-facing command for the dd VM-less container runtime.
//!
//! It launches the `dd-app` GUI, runs/controls the per-user daemon LaunchAgent, and wires up a
//! `docker context` so the stock `docker` CLI can talk to dd — all without root.
//!
//!   dd install        # set up ~/.dd, load the daemon agent, create the docker context
//!   dd app            # open the GUI
//!   dd doctor         # check everything is healthy

mod agent;
mod context;
mod paths;

use clap::{Parser, Subcommand};
use std::process::Command;

#[derive(Parser)]
#[command(name = "dd", version, about = "dd — VM-less containers on macOS")]
struct Cli {
    #[command(subcommand)]
    cmd: Cmd,
}

#[derive(Subcommand)]
enum Cmd {
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
        Cmd::App => cmd_app(),
        Cmd::Daemon { action } => cmd_daemon(action),
        Cmd::Install => cmd_install(),
        Cmd::Uninstall { purge } => cmd_uninstall(purge),
        Cmd::Context { action } => cmd_context(action),
        Cmd::Doctor => cmd_doctor(),
    };
    std::process::exit(code);
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
    println!("\nDone. Try:  dd doctor");
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
        println!("\nSome checks failed. `dd install` sets up the agent + context.");
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

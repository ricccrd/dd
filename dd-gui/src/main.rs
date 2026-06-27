//! dd-app — a GTK4 desktop UI for the **dd** VM-less container runtime.
//!
//! Container-centric master/detail: a left sidebar lists containers + images (with the daemon
//! connection shown as a sidebar footer), and the content pane shows the selected item's detail —
//! a container's image/status/volumes/networks/ports/logs, or an image's run action. It is a thin
//! Docker-Engine-API client (`ddclient`) over the daemon's Unix socket, polled every couple seconds.
//!
//! Built only on macOS where the GTK stack is available (see the workspace `default-members` note).

mod ui;
mod update;
#[cfg(target_os = "macos")]
mod mac;

use ddclient::{Client, Container, Image, Network};
use gtk::prelude::GtkWindowExt; // for root.set_default_size on connect/disconnect
use relm4::prelude::*;
use std::path::PathBuf;
use std::time::Duration;

/// Top-level navigation category (first sidebar).
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub enum Category {
    #[default]
    Home,
    Containers,
    Images,
    Settings,
}

/// What the detail pane is currently showing.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub enum Selection {
    #[default]
    None,
    Container(String),
    Image(String),
}

/// A full snapshot of daemon state, fetched off the UI thread.
#[derive(Debug, Default)]
struct Snapshot {
    connected: bool,
    containers: Vec<Container>,
    images: Vec<Image>,
    networks: Vec<Network>,
    /// Active `docker` context name, or `None` if the docker CLI isn't installed.
    docker_context: Option<String>,
    /// All selectable `docker` contexts (always includes `dd`).
    docker_contexts: Vec<String>,
}

/// Messages from the UI (selection, button clicks, the refresh tick).
#[derive(Debug)]
enum Msg {
    ToggleDaemon,
    InstallCli,
    ConfirmReset,
    Reset,
    UpdateFound(update::Release),
    ApplyUpdate,
    SetContext(String),
    SetCategory(Category),
    Select(Selection),
    RunImage(String),
    StartContainer(String),
    RemoveContainer(String),
    RemoveImage(String),
}

/// Results delivered back to the UI thread from async work.
#[derive(Debug)]
enum Cmd {
    Snapshot(Box<Snapshot>),
    Logs(String, String),
}

/// The application model.
struct AppModel {
    socket: PathBuf,
    snap: Snapshot,
    category: Category,
    selection: Selection,
    /// Logs for the currently selected container: `(container id, text)`.
    current_logs: Option<(String, String)>,
    /// Tracks connection transitions so we resize the window only when it flips.
    was_connected: bool,
    /// A newer release found on GitHub, if any (drives the "Update" button).
    update: Option<update::Release>,
    /// The daemon process we started (so we can stop it), if any.
    daemon_child: Option<std::process::Child>,
    /// Whether we've already offered to switch the docker context this session.
    context_prompted: bool,
}

impl Component for AppModel {
    type Init = PathBuf;
    type Input = Msg;
    type Output = ();
    type CommandOutput = Cmd;
    type Root = gtk::ApplicationWindow;
    type Widgets = ui::Widgets;

    fn init_root() -> Self::Root {
        gtk::ApplicationWindow::new(&relm4::main_application())
    }

    fn init(socket: Self::Init, root: Self::Root, sender: ComponentSender<Self>) -> ComponentParts<Self> {
        let model = AppModel {
            socket: socket.clone(),
            snap: Snapshot::default(),
            category: Category::Home,
            selection: Selection::None,
            current_logs: None,
            was_connected: false,
            update: None,
            daemon_child: None,
            context_prompted: false,
        };
        let widgets = ui::build(&root, &sender);

        // Seed ~/.dd/images with the bundled starter image(s) so a novice has something to run.
        seed_images();

        // One-shot update check on startup (off the UI thread).
        {
            let sender = sender.clone();
            std::thread::spawn(move || {
                if let Some(rel) = update::check(env!("DD_VERSION")) {
                    sender.input(Msg::UpdateFound(rel));
                }
            });
        }

        // Background poll loop: fetch a snapshot every 2s until the component shuts down.
        sender.command(move |out, shutdown| {
            let socket = socket.clone();
            shutdown
                .register(async move {
                    let client = Client::new(&socket);
                    loop {
                        let snap = fetch(&client).await;
                        if out.send(Cmd::Snapshot(Box::new(snap))).is_err() {
                            break;
                        }
                        tokio::time::sleep(Duration::from_secs(2)).await;
                    }
                })
                .drop_on_shutdown()
        });

        ComponentParts { model, widgets }
    }

    fn update(&mut self, msg: Msg, sender: ComponentSender<Self>, root: &Self::Root) {
        let socket = self.socket.clone();
        match msg {
            Msg::ToggleDaemon => {
                let delay = if self.snap.connected {
                    // Stop: kill the daemon we started, else bootout an installed LaunchAgent.
                    match self.daemon_child.take() {
                        Some(mut child) => {
                            let _ = child.kill();
                        }
                        None => stop_external_daemon(),
                    }
                    600
                } else {
                    self.daemon_child = spawn_daemon();
                    1300
                };
                sender.oneshot_command(async move {
                    tokio::time::sleep(Duration::from_millis(delay)).await;
                    Cmd::Snapshot(Box::new(fetch(&Client::new(&socket)).await))
                });
            }
            Msg::InstallCli => ui::show_cli_install(root),
            Msg::ConfirmReset => ui::confirm_reset(root, &sender),
            Msg::Reset => {
                self.selection = Selection::None;
                self.current_logs = None;
                sender.oneshot_command(async move {
                    let c = Client::new(&socket);
                    if let Ok(cs) = c.list_containers().await {
                        for ct in cs {
                            let _ = c.remove_container(&ct.id).await;
                        }
                    }
                    if let Ok(vs) = c.list_volumes().await {
                        for v in vs {
                            let _ = c.remove_volume(&v.name).await;
                        }
                    }
                    if let Ok(ns) = c.list_networks().await {
                        for n in ns {
                            if !matches!(n.name.as_str(), "bridge" | "host" | "none") {
                                let _ = c.remove_network(&n.id).await;
                            }
                        }
                    }
                    Cmd::Snapshot(Box::new(fetch(&c).await))
                });
            }
            Msg::UpdateFound(rel) => self.update = Some(rel),
            Msg::ApplyUpdate => {
                if let Some(rel) = self.update.clone() {
                    std::thread::spawn(move || match update::install(&rel) {
                        Ok(()) => std::process::exit(0), // the freshly-installed copy is launching
                        Err(e) => eprintln!("update failed: {e}"),
                    });
                }
            }
            Msg::SetContext(name) => {
                sender.oneshot_command(async move {
                    set_context(&name, &socket).await;
                    tokio::time::sleep(Duration::from_millis(250)).await;
                    Cmd::Snapshot(Box::new(fetch(&Client::new(&socket)).await))
                });
            }
            Msg::SetCategory(cat) => {
                if self.category != cat {
                    self.category = cat;
                    self.selection = Selection::None; // fresh pick from the new category
                    self.current_logs = None;
                }
            }
            Msg::Select(sel) => {
                self.selection = sel.clone();
                self.current_logs = None;
                if let Selection::Container(id) = sel {
                    fetch_logs(&sender, self.socket.clone(), id);
                }
            }
            Msg::RunImage(image) => {
                // Jump to Containers so the user sees the new one appear.
                self.category = Category::Containers;
                self.selection = Selection::None;
                self.act(sender, socket, move |c| async move {
                    let spec = ddclient::CreateContainer { image, ..Default::default() };
                    if let Ok(id) = c.create_container(&spec).await {
                        let _ = c.start_container(&id).await;
                    }
                });
            }
            Msg::StartContainer(id) => self.act(sender, socket, move |c| async move {
                let _ = c.start_container(&id).await;
            }),
            Msg::RemoveContainer(id) => {
                if self.selection == Selection::Container(id.clone()) {
                    self.selection = Selection::None;
                }
                self.current_logs = None;
                self.act(sender, socket, move |c| async move {
                    let _ = c.remove_container(&id).await;
                });
            }
            Msg::RemoveImage(name) => {
                if self.selection == Selection::Image(name.clone()) {
                    self.selection = Selection::None;
                }
                self.act(sender, socket, move |c| async move {
                    let _ = c.remove_image(&name).await;
                });
            }
        }
    }

    fn update_cmd(&mut self, cmd: Cmd, sender: ComponentSender<Self>, root: &Self::Root) {
        match cmd {
            Cmd::Snapshot(s) => {
                self.snap = *s;
                // Compact onboarding window when the daemon is off; expand when it comes up.
                if self.snap.connected != self.was_connected {
                    self.was_connected = self.snap.connected;
                    if self.snap.connected {
                        root.set_default_size(1040, 680);
                    } else {
                        root.set_default_size(660, 420);
                    }
                }
                // Offer once, on first data, to point the docker CLI at our daemon.
                if !self.context_prompted {
                    self.context_prompted = true;
                    if let Some(ctx) = self.snap.docker_context.clone() {
                        if ctx != "dd" {
                            ui::prompt_switch_context(root, &sender, &ctx);
                        }
                    }
                }
                // Auto-select the first item of the current category if nothing is selected.
                if self.selection == Selection::None {
                    match self.category {
                        Category::Home | Category::Settings => {}
                        Category::Containers => {
                            if let Some(c) = self.snap.containers.first() {
                                self.selection = Selection::Container(c.id.clone());
                                fetch_logs(&sender, self.socket.clone(), c.id.clone());
                            }
                        }
                        Category::Images => {
                            if let Some(i) = self.snap.images.first() {
                                self.selection = Selection::Image(i.name());
                            }
                        }
                    }
                } else if let Selection::Container(id) = &self.selection {
                    // Keep the selected container's logs fresh.
                    fetch_logs(&sender, self.socket.clone(), id.clone());
                }
            }
            Cmd::Logs(id, text) => {
                if let Selection::Container(sel) = &self.selection {
                    if sel == &id {
                        self.current_logs = Some((id, last_lines(&text, 1000)));
                    }
                }
            }
        }
    }

    fn update_view(&self, widgets: &mut Self::Widgets, sender: ComponentSender<Self>) {
        ui::render(widgets, self, &sender);
    }
}

impl AppModel {
    /// Run a mutating action against the daemon, then refresh the snapshot — all off-thread.
    fn act<F, Fut>(&self, sender: ComponentSender<Self>, socket: PathBuf, f: F)
    where
        F: FnOnce(Client) -> Fut + Send + 'static,
        Fut: std::future::Future<Output = ()> + Send,
    {
        sender.oneshot_command(async move {
            let client = Client::new(&socket);
            f(client.clone()).await;
            Cmd::Snapshot(Box::new(fetch(&client).await))
        });
    }
}

/// Symlink the bundled `dd` CLI into `~/.local/bin` (no root). Returns `(link path, already on
/// PATH)`. The onboarding window turns this into per-shell instructions.
pub(crate) fn install_cli() -> Result<(PathBuf, bool), String> {
    let cli = resolve_cli().ok_or("dd CLI binary not found in the app bundle")?;
    let name = cli.file_name().ok_or("bad CLI path")?;
    let home = std::env::var("HOME").map_err(|_| "no HOME".to_string())?;
    let bindir = PathBuf::from(&home).join(".local/bin");
    std::fs::create_dir_all(&bindir).map_err(|e| e.to_string())?;
    let link = bindir.join(name);
    let _ = std::fs::remove_file(&link);
    std::os::unix::fs::symlink(&cli, &link).map_err(|e| e.to_string())?;

    let on_path = std::env::var("PATH")
        .unwrap_or_default()
        .split(':')
        .any(|p| p == bindir.to_string_lossy());
    Ok((link, on_path))
}

/// Locate the bundled `dd` CLI: `$DD_CLI_BIN`, the app bundle, or a sibling of this binary (dev).
fn resolve_cli() -> Option<PathBuf> {
    if let Some(p) = std::env::var_os("DD_CLI_BIN") {
        return Some(PathBuf::from(p));
    }
    let names = ["ddcli", "dd"]; // whichever the CLI is built as
    // Prefer the *installed* bundle so the symlink stays valid across relaunches and updates
    // (which replace /Applications/dd-app.app in place), not the dev copy we run from.
    for n in names {
        let p = PathBuf::from("/Applications/dd-app.app/Contents/Resources").join(n);
        if p.exists() {
            return Some(p);
        }
    }
    let exe = std::env::current_exe().ok()?;
    if let Some(contents) = exe.parent().and_then(|p| p.parent()) {
        for n in names {
            let c = contents.join("Resources").join(n);
            if c.exists() {
                return Some(c);
            }
        }
    }
    let dir = exe.parent()?;
    names.iter().map(|n| dir.join(n)).find(|p| p.exists())
}

/// Copy any bundled starter images into `~/.dd/images` (skipping ones already there).
fn seed_images() {
    let Some(src) = bundled_images_dir() else { return };
    let home = std::env::var("HOME").unwrap_or_else(|_| ".".into());
    let dest = PathBuf::from(home).join(".dd/images");
    let _ = std::fs::create_dir_all(&dest);
    let Ok(rd) = std::fs::read_dir(&src) else { return };
    for e in rd.flatten() {
        if e.path().is_dir() && !dest.join(e.file_name()).exists() {
            let _ = std::process::Command::new("cp").arg("-R").arg(e.path()).arg(&dest).output();
        }
    }
}

/// The bundled images dir: `Contents/Resources/images`, or `assets/images` in dev.
fn bundled_images_dir() -> Option<PathBuf> {
    if let Ok(exe) = std::env::current_exe() {
        if let Some(contents) = exe.parent().and_then(|p| p.parent()) {
            let p = contents.join("Resources/images");
            if p.exists() {
                return Some(p);
            }
        }
    }
    let dev = PathBuf::from("assets/images");
    dev.exists().then_some(dev)
}

/// Fetch a container's logs off-thread and deliver them as `Cmd::Logs`.
fn fetch_logs(sender: &ComponentSender<AppModel>, socket: PathBuf, id: String) {
    sender.oneshot_command(async move {
        let text = match Client::new(&socket).container_logs(&id).await {
            Ok(b) => String::from_utf8_lossy(&b).into_owned(),
            Err(e) => format!("could not fetch logs: {e}"),
        };
        Cmd::Logs(id, text)
    });
}

/// Keep only the last `n` lines of `text`.
fn last_lines(text: &str, n: usize) -> String {
    let lines: Vec<&str> = text.lines().collect();
    let start = lines.len().saturating_sub(n);
    lines[start..].join("\n")
}

/// Fetch a full snapshot. A failed `ping` short-circuits to a disconnected snapshot, but we still
/// report the active docker context (independent of whether our daemon is up).
async fn fetch(c: &Client) -> Snapshot {
    let docker_context = docker_context().await;
    let docker_contexts = if docker_context.is_some() { docker_contexts().await } else { vec![] };
    if c.ping().await.is_err() {
        return Snapshot { docker_context, docker_contexts, ..Snapshot::default() };
    }
    Snapshot {
        connected: true,
        containers: c.list_containers().await.unwrap_or_default(),
        images: c.list_images().await.unwrap_or_default(),
        networks: c.list_networks().await.unwrap_or_default(),
        docker_context,
        docker_contexts,
    }
}

/// The active `docker` context name, or `None` if the docker CLI isn't installed / errored.
async fn docker_context() -> Option<String> {
    let out = tokio::process::Command::new("docker").args(["context", "show"]).output().await.ok()?;
    if !out.status.success() {
        return None;
    }
    let s = String::from_utf8_lossy(&out.stdout).trim().to_string();
    (!s.is_empty()).then_some(s)
}

/// All selectable docker contexts (`docker context ls`), always including `dd` so the user can
/// pick our daemon even before the context exists.
async fn docker_contexts() -> Vec<String> {
    let out = tokio::process::Command::new("docker")
        .args(["context", "ls", "--format", "{{.Name}}"])
        .output()
        .await;
    let mut list: Vec<String> = match out {
        Ok(o) if o.status.success() => String::from_utf8_lossy(&o.stdout)
            .lines()
            .map(|s| s.trim().to_string())
            .filter(|s| !s.is_empty())
            .collect(),
        _ => vec![],
    };
    if !list.iter().any(|c| c == "dd") {
        list.push("dd".to_string());
    }
    list
}

/// Switch the `docker` CLI to context `name` (creating the `dd` context first if needed).
async fn set_context(name: &str, socket: &std::path::Path) {
    use tokio::process::Command;
    if name == "dd" {
        let host = format!("host=unix://{}", socket.display());
        let _ = Command::new("docker").args(["context", "create", "dd", "--docker", &host]).output().await;
    }
    let _ = Command::new("docker").args(["context", "use", name]).output().await;
}

/// Spawn the dd-daemon binary detached, with the canonical socket/images/JIT env, and return the
/// child so we can stop it later. Resolves the binary from `$DD_DAEMON_BIN`, the app bundle
/// (`Contents/Resources/dd-daemon`), or a sibling of this executable (the dev/`cargo` layout).
fn spawn_daemon() -> Option<std::process::Child> {
    use std::process::{Command, Stdio};
    let home = std::env::var("HOME").unwrap_or_else(|_| ".".into());
    let dd = PathBuf::from(&home).join(".dd");
    let run = dd.join("run");
    let images = dd.join("images");
    let _ = std::fs::create_dir_all(&run);
    let _ = std::fs::create_dir_all(&images);

    let (bin, jit_dir) = resolve_daemon()?;
    Command::new(&bin)
        .env("DDOCKERD_SOCK", run.join("docker.sock"))
        .env("DD_IMAGES", &images)
        .env("DDJIT_DIR", &jit_dir)
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .spawn()
        .ok()
}

/// Stop a daemon we didn't start: if it's an installed LaunchAgent, bootout the per-user service.
fn stop_external_daemon() {
    extern "C" {
        fn getuid() -> u32;
    }
    let uid = unsafe { getuid() };
    let _ = std::process::Command::new("launchctl")
        .args(["bootout", &format!("gui/{uid}/com.dd.daemon")])
        .output();
}

/// Locate the daemon binary and the dir holding the `ddjit-*` engines.
fn resolve_daemon() -> Option<(PathBuf, PathBuf)> {
    if let Some(p) = std::env::var_os("DD_DAEMON_BIN") {
        let p = PathBuf::from(p);
        let dir = p.parent().map(|d| d.to_path_buf()).unwrap_or_default();
        return Some((p, dir));
    }
    let exe = std::env::current_exe().ok()?;
    // Bundle: .../Contents/MacOS/dd-app -> .../Contents/Resources/dd-daemon
    if let Some(contents) = exe.parent().and_then(|p| p.parent()) {
        let res = contents.join("Resources");
        let cand = res.join("dd-daemon");
        if cand.exists() {
            return Some((cand, res));
        }
    }
    // Dev: a dd-daemon next to this binary (ddjit-* paths are baked in at compile time there).
    if let Some(dir) = exe.parent() {
        let cand = dir.join("dd-daemon");
        if cand.exists() {
            return Some((cand, dir.to_path_buf()));
        }
    }
    None
}

fn main() {
    ui::setup_bundle_env();
    let socket = Client::default_socket();
    let app = RelmApp::new("com.dd.app");
    app.run::<AppModel>(socket);
}

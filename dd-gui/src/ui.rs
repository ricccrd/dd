//! Widget construction and rendering for dd-app (pure GTK4). Container-centric master/detail:
//! a left sidebar (containers + images, with the daemon connection as its footer) drives a detail
//! pane. `build()` lays out the static shell once; `render()` repopulates the sidebar + detail
//! from each new snapshot.

use crate::{AppModel, Category, Msg, Selection};
use ddclient::{Container, Image, Network};
use gtk::prelude::*;
use relm4::ComponentSender;
use std::ffi::OsStr;

/// Widget handles the component needs across renders. Three panes: category nav, item list, and
/// the detail pane (info on top, a fixed logs area pinned at the bottom).
pub struct Widgets {
    stack: gtk::Stack,
    update_btn: gtk::Button,
    onboard_status: gtk::Label,
    nav: gtk::ListBox,
    nav_bottom: gtk::ListBox,
    content_stack: gtk::Stack,
    home: gtk::Box,
    settings: gtk::Box,
    list: gtk::ListBox,
    detail_info: gtk::Box,
    logs_area: gtk::Box,
    logs_view: gtk::TextView,
    daemon_dot: gtk::Box,
    daemon_label: gtk::Label,
    daemon_toggle: gtk::Button,
    context_menu: gtk::MenuButton,
    context_pop_box: gtk::Box,
    context_seg: gtk::Box,
}

/// Build the window shell: a frameless-ish flat header, and one floating pane holding the
/// [sidebar | detail] split.
pub fn build(root: &gtk::ApplicationWindow, sender: &ComponentSender<AppModel>) -> Widgets {
    load_css();
    root.set_title(Some("dd"));
    root.set_default_size(660, 420); // compact onboarding size; expands when the daemon comes up

    // Status control (daemon toggle + docker selector). We deliberately DON'T use a GtkHeaderBar:
    // that draws GTK's cross-platform window buttons (the "Linux-like" ones). Without it the window
    // keeps the NATIVE macOS title bar with real traffic-light controls, and we place the status in
    // a slim strip just below it (assembled at the end of build()).
    let daemon_dot = gtk::Box::new(gtk::Orientation::Horizontal, 0); // a CSS-drawn status circle
    daemon_dot.add_css_class("dd-dot");
    daemon_dot.set_valign(gtk::Align::Center);
    let daemon_label = gtk::Label::new(Some("Daemon"));
    let daemon_box = gtk::Box::new(gtk::Orientation::Horizontal, 7);
    daemon_box.append(&daemon_dot);
    daemon_box.append(&daemon_label);
    let daemon_toggle = gtk::Button::builder().child(&daemon_box).build();
    daemon_toggle.set_has_frame(false);
    daemon_toggle.add_css_class("dd-seg");
    {
        let s = sender.clone();
        daemon_toggle.connect_clicked(move |_| s.input(Msg::ToggleDaemon));
    }

    // Docker control: a selector of available docker contexts (daemons). Click to choose one.
    let context_pop_box = gtk::Box::new(gtk::Orientation::Vertical, 1);
    context_pop_box.set_size_request(150, -1);
    let context_pop = gtk::Popover::new();
    context_pop.set_has_arrow(false);
    context_pop.add_css_class("dd-pop");
    context_pop.set_child(Some(&context_pop_box));
    let context_menu = gtk::MenuButton::new();
    context_menu.set_label("Docker");
    context_menu.set_has_frame(false);
    context_menu.set_popover(Some(&context_pop));
    context_menu.add_css_class("dd-seg");
    let context_seg = gtk::Box::new(gtk::Orientation::Horizontal, 0);
    context_seg.append(&context_menu);
    context_seg.set_visible(false); // shown by render() when the docker CLI is present

    let group = gtk::Box::new(gtk::Orientation::Horizontal, 2);
    group.add_css_class("dd-statusgroup");
    group.set_valign(gtk::Align::Center);
    group.set_halign(gtk::Align::End);
    group.set_hexpand(true);
    group.append(&daemon_toggle);
    group.append(&context_seg);

    // --- pane 1: category nav (Home / Containers / Images; Settings pinned bottom) ---
    let nav = nav_list();
    nav.append(&cat_row("go-home-symbolic", "Home"));
    nav.append(&cat_row("package-x-generic-symbolic", "Containers"));
    nav.append(&cat_row("image-x-generic-symbolic", "Images"));
    let nav_bottom = nav_list();
    nav_bottom.append(&cat_row("emblem-system-symbolic", "Settings"));
    {
        let s = sender.clone();
        let other = nav_bottom.clone();
        nav.connect_row_activated(move |_, row| {
            other.unselect_all();
            let cat = match row.index() {
                1 => Category::Containers,
                2 => Category::Images,
                _ => Category::Home,
            };
            s.input(Msg::SetCategory(cat));
        });
    }
    {
        let s = sender.clone();
        let other = nav.clone();
        nav_bottom.connect_row_activated(move |_, _| {
            other.unselect_all();
            s.input(Msg::SetCategory(Category::Settings));
        });
    }
    let nav_card = sidebar_card();
    nav_card.set_size_request(150, -1);
    nav_card.append(&nav);
    let spacer = gtk::Box::new(gtk::Orientation::Vertical, 0);
    spacer.set_vexpand(true);
    nav_card.append(&spacer);
    nav_card.append(&nav_bottom);

    // --- pane 2: the items in the selected category ------------------------
    let list = nav_list();
    {
        let s = sender.clone();
        list.connect_row_activated(move |_, row| {
            let wn = row.widget_name();
            if let Some(id) = wn.as_str().strip_prefix("c:") {
                s.input(Msg::Select(Selection::Container(id.to_string())));
            } else if let Some(name) = wn.as_str().strip_prefix("i:") {
                s.input(Msg::Select(Selection::Image(name.to_string())));
            }
        });
    }
    let list_scroll = gtk::ScrolledWindow::builder()
        .child(&list)
        .hscrollbar_policy(gtk::PolicyType::Never)
        .vexpand(true)
        .build();
    let list_card = sidebar_card();
    list_card.append(&list_scroll);

    // --- pane 3: detail (info on top, logs pinned at the bottom) -----------
    let detail_info = gtk::Box::new(gtk::Orientation::Vertical, 0);
    detail_info.set_hexpand(true);
    let info_scroll = gtk::ScrolledWindow::builder()
        .child(&detail_info)
        .hscrollbar_policy(gtk::PolicyType::Never)
        .vexpand(true)
        .hexpand(true)
        .build();

    let logs_view = gtk::TextView::builder().editable(false).monospace(true).cursor_visible(false).build();
    logs_view.set_left_margin(10);
    logs_view.set_right_margin(10);
    logs_view.set_top_margin(6);
    logs_view.set_bottom_margin(6);
    logs_view.add_css_class("dd-logs");
    let logs_scroll = gtk::ScrolledWindow::builder().child(&logs_view).vexpand(false).build();
    logs_scroll.set_size_request(-1, 220); // fixed-height log pane at the bottom
    let logs_area = gtk::Box::new(gtk::Orientation::Vertical, 0);
    logs_area.append(&gtk::Separator::new(gtk::Orientation::Horizontal));
    let logs_cap = gtk::Label::new(Some("LOGS"));
    logs_cap.set_xalign(0.0);
    logs_cap.set_halign(gtk::Align::Start);
    logs_cap.add_css_class("dd-section-title");
    logs_cap.set_margin_start(14);
    logs_cap.set_margin_top(8);
    logs_cap.set_margin_bottom(4);
    logs_area.append(&logs_cap);
    logs_area.append(&logs_scroll);

    let detail_card = gtk::Box::new(gtk::Orientation::Vertical, 0);
    detail_card.add_css_class("dd-content");
    detail_card.set_overflow(gtk::Overflow::Hidden);
    detail_card.set_size_request(360, -1);
    detail_card.append(&info_scroll);
    detail_card.append(&logs_area);

    // list | detail are resizable; nav stays fixed.
    let paned = gtk::Paned::new(gtk::Orientation::Horizontal);
    paned.set_start_child(Some(&list_card));
    paned.set_end_child(Some(&detail_card));
    paned.set_position(220);
    paned.set_resize_start_child(false);
    paned.set_shrink_start_child(false);
    paned.set_shrink_end_child(false);
    paned.set_wide_handle(false); // a thin handle, so the list↔detail gap matches the nav↔list gap
    paned.set_hexpand(true);

    // Home dashboard (filled by render()).
    let home = gtk::Box::new(gtk::Orientation::Vertical, 16);
    home.set_margin_top(20);
    home.set_margin_bottom(20);
    home.set_margin_start(22);
    home.set_margin_end(22);
    let home_scroll = gtk::ScrolledWindow::builder()
        .child(&home)
        .hscrollbar_policy(gtk::PolicyType::Never)
        .vexpand(true)
        .hexpand(true)
        .build();
    home_scroll.add_css_class("dd-home");

    // Settings page (filled by render()).
    let settings = gtk::Box::new(gtk::Orientation::Vertical, 16);
    settings.set_margin_top(20);
    settings.set_margin_bottom(20);
    settings.set_margin_start(22);
    settings.set_margin_end(22);
    let settings_scroll = gtk::ScrolledWindow::builder()
        .child(&settings)
        .hscrollbar_policy(gtk::PolicyType::Never)
        .vexpand(true)
        .hexpand(true)
        .build();
    settings_scroll.add_css_class("dd-home");

    // Content area: dashboard ("home") / settings / browse (list | detail).
    let content_stack = gtk::Stack::new();
    content_stack.set_hexpand(true);
    content_stack.add_named(&home_scroll, Some("home"));
    content_stack.add_named(&settings_scroll, Some("settings"));
    content_stack.add_named(&paned, Some("browse"));

    let body = gtk::Box::new(gtk::Orientation::Horizontal, 7);
    body.set_margin_top(0);
    body.set_margin_bottom(7);
    body.set_margin_start(7);
    body.set_margin_end(7);
    body.append(&nav_card);
    body.append(&content_stack);

    // Onboarding / splash screen, shown when the daemon is off.
    let (onboarding, onboard_status) = build_onboarding(sender);

    // Two views: compact onboarding vs. the full app.
    let stack = gtk::Stack::new();
    stack.set_transition_type(gtk::StackTransitionType::Crossfade);
    stack.add_named(&onboarding, Some("onboarding"));
    stack.add_named(&body, Some("main"));

    // "Update available" button (hidden unless a newer release is found).
    let update_btn = gtk::Button::new();
    update_btn.add_css_class("dd-update");
    update_btn.set_valign(gtk::Align::Center);
    update_btn.set_visible(false);
    {
        let s = sender.clone();
        update_btn.connect_clicked(move |_| s.input(Msg::ApplyUpdate));
    }

    // Slim status strip (under the native title bar) above the body.
    let strip = gtk::Box::new(gtk::Orientation::Horizontal, 0);
    strip.add_css_class("dd-topstrip");
    strip.set_size_request(-1, 38); // title-bar height; traffic lights float over the (empty) left
    strip.set_margin_start(12);
    strip.set_margin_end(7);
    strip.append(&update_btn);
    strip.append(&group);

    let outer = gtk::Box::new(gtk::Orientation::Vertical, 0);
    outer.append(&strip);
    outer.append(&stack);
    root.set_child(Some(&outer));

    // Unify the title bar with our content (native traffic lights float over the strip).
    #[cfg(target_os = "macos")]
    root.connect_realize(|_| crate::mac::unify_titlebar());

    Widgets {
        stack, update_btn, onboard_status,
        nav, nav_bottom, content_stack, home, settings, list, detail_info, logs_area, logs_view,
        daemon_dot, daemon_label, daemon_toggle, context_menu, context_pop_box, context_seg,
    }
}

/// The onboarding/splash view: a 2-column layout — branding + enable on the left, CLI install on
/// the right.
fn build_onboarding(sender: &ComponentSender<AppModel>) -> (gtk::Widget, gtk::Label) {
    // Left column: status + enable the daemon.
    let head = gtk::Label::new(Some("Daemon"));
    head.set_xalign(0.0);
    head.add_css_class("dd-onboard-head");
    let subtitle = gtk::Label::new(Some("Run Linux containers on macOS — no VM."));
    subtitle.set_xalign(0.0);
    subtitle.set_wrap(true);
    subtitle.add_css_class("dd-sub");
    let onboard_status = gtk::Label::new(Some("The dd daemon is not running."));
    onboard_status.set_xalign(0.0);
    onboard_status.add_css_class("dd-onboard-status");
    let enable = gtk::Button::with_label("Enable daemon");
    enable.add_css_class("dd-btn");
    enable.add_css_class("suggested-action");
    enable.set_halign(gtk::Align::Start);
    enable.set_margin_top(4);
    {
        let s = sender.clone();
        enable.connect_clicked(move |_| s.input(Msg::ToggleDaemon));
    }
    let left = gtk::Box::new(gtk::Orientation::Vertical, 7);
    left.set_valign(gtk::Align::Center);
    left.set_size_request(250, -1);
    left.append(&head);
    left.append(&subtitle);
    left.append(&onboard_status);
    left.append(&enable);

    // Right column: install the CLI.
    let cli_head = gtk::Label::new(Some("Command-line tool"));
    cli_head.set_xalign(0.0);
    cli_head.add_css_class("dd-onboard-head");
    let cli_desc = gtk::Label::new(Some("Install the dd command to manage containers from your terminal."));
    cli_desc.set_xalign(0.0);
    cli_desc.set_wrap(true);
    cli_desc.add_css_class("dd-sub");
    let install = gtk::Button::with_label("Install dd CLI");
    install.add_css_class("dd-btn");
    install.set_halign(gtk::Align::Start);
    install.set_margin_top(4);
    {
        let s = sender.clone();
        install.connect_clicked(move |_| s.input(Msg::InstallCli));
    }
    let right = gtk::Box::new(gtk::Orientation::Vertical, 7);
    right.set_valign(gtk::Align::Center);
    right.set_size_request(250, -1);
    right.append(&cli_head);
    right.append(&cli_desc);
    right.append(&install);

    let row = gtk::Box::new(gtk::Orientation::Horizontal, 28);
    row.set_halign(gtk::Align::Center);
    row.set_hexpand(true);
    row.append(&left);
    row.append(&gtk::Separator::new(gtk::Orientation::Vertical));
    row.append(&right);

    // Logo: above the columns, aligned to the left, small. gtk::Image honours a fixed pixel-size
    // (unlike Picture, whose size_request is only a minimum and would span the width).
    let top = gtk::Box::new(gtk::Orientation::Horizontal, 0);
    top.set_hexpand(true);
    if let Some(p) = logo_path() {
        let img = gtk::Image::from_file(&p);
        img.set_pixel_size(56);
        img.set_halign(gtk::Align::Start);
        img.set_margin_bottom(14); // padding between the logo and the columns below
        top.append(&img);
    }

    let outer = gtk::Box::new(gtk::Orientation::Vertical, 8);
    outer.set_valign(gtk::Align::Center);
    outer.set_margin_start(30);
    outer.set_margin_end(30);
    outer.set_margin_top(8);
    outer.set_margin_bottom(8);
    outer.append(&top);
    outer.append(&row);
    (outer.upcast(), onboard_status)
}

/// Locate the dd logo: the app bundle's `Contents/Resources/logo.png`, or `assets/logo.png` in dev.
fn logo_path() -> Option<std::path::PathBuf> {
    if let Ok(exe) = std::env::current_exe() {
        if let Some(contents) = exe.parent().and_then(|p| p.parent()) {
            let p = contents.join("Resources/logo.png");
            if p.exists() {
                return Some(p);
            }
        }
    }
    let dev = std::path::PathBuf::from("assets/logo.png");
    dev.exists().then_some(dev)
}

/// A sidebar-style floating card (translucent, rounded, clipped).
fn sidebar_card() -> gtk::Box {
    let b = gtk::Box::new(gtk::Orientation::Vertical, 0);
    b.add_css_class("dd-sidebar");
    b.set_overflow(gtk::Overflow::Hidden);
    b
}

/// Repopulate the three panes (nav, item list, detail + logs) from the model.
pub fn render(w: &Widgets, m: &AppModel, sender: &ComponentSender<AppModel>) {
    let snap = &m.snap;

    // Update affordance.
    match &m.update {
        Some(rel) => {
            w.update_btn.set_visible(true);
            w.update_btn.set_label(&format!("Update v{}", rel.version));
            w.update_btn.set_tooltip_text(Some("Download and install the new version"));
        }
        None => w.update_btn.set_visible(false),
    }

    // Onboarding (compact) vs. the full app.
    w.stack.set_visible_child_name(if snap.connected { "main" } else { "onboarding" });
    w.onboard_status.set_label(if snap.connected {
        "The dd daemon is running."
    } else {
        "The dd daemon is not running."
    });

    // Pane 1: reflect the active category across the top + bottom (Settings) nav lists.
    if m.category == Category::Settings {
        w.nav.unselect_all();
        w.nav_bottom.select_row(w.nav_bottom.row_at_index(0).as_ref());
    } else {
        w.nav_bottom.unselect_all();
        let idx = match m.category {
            Category::Containers => 1,
            Category::Images => 2,
            _ => 0,
        };
        w.nav.select_row(w.nav.row_at_index(idx).as_ref());
    }

    if m.category == Category::Home {
        w.content_stack.set_visible_child_name("home");
        render_home(&w.home, m, sender);
    } else if m.category == Category::Settings {
        w.content_stack.set_visible_child_name("settings");
        render_settings(&w.settings, m, sender);
    } else {
        w.content_stack.set_visible_child_name("browse");

        // Pane 2: the items in the active category.
        clear(&w.list);
        match m.category {
            Category::Containers => {
                if snap.containers.is_empty() {
                    w.list.append(&dim_row(if snap.connected { "No containers" } else { "—" }));
                }
                for c in &snap.containers {
                    let row = container_list_row(c);
                    row.set_widget_name(&format!("c:{}", c.id));
                    w.list.append(&row);
                }
            }
            Category::Images => {
                if snap.images.is_empty() {
                    w.list.append(&dim_row("No images"));
                }
                for img in &snap.images {
                    let row = nav_item(&img.name(), &img.architecture, false);
                    row.set_widget_name(&format!("i:{}", img.name()));
                    w.list.append(&row);
                }
            }
            Category::Home | Category::Settings => {}
        }
        w.list.unselect_all();
        match &m.selection {
            Selection::Container(id) => select_named(&w.list, &format!("c:{id}")),
            Selection::Image(name) => select_named(&w.list, &format!("i:{name}")),
            Selection::None => {}
        }

        // Pane 3: detail info.
        clear_box(&w.detail_info);
        let info = match &m.selection {
            Selection::Container(id) => match snap.containers.iter().find(|c| &c.id == id) {
                Some(c) => container_info(c, &snap.networks, sender),
                None => placeholder("This container no longer exists."),
            },
            Selection::Image(name) => match snap.images.iter().find(|i| &i.name() == name) {
                Some(img) => image_detail(img, sender),
                None => placeholder("This image no longer exists."),
            },
            Selection::None => placeholder(if snap.connected { "Select an item." } else { "Daemon not running." }),
        };
        w.detail_info.append(&info);

        // Logs pane (only for containers; auto-rendered, scrolled to the end).
        if let Selection::Container(id) = &m.selection {
            w.logs_area.set_visible(true);
            let text = match &m.current_logs {
                Some((lid, t)) if lid == id => {
                    if t.is_empty() { "(no output)".to_string() } else { t.clone() }
                }
                _ => "loading…".to_string(),
            };
            let buf = w.logs_view.buffer();
            buf.set_text(&text);
            let mut end = buf.end_iter();
            w.logs_view.scroll_to_iter(&mut end, 0.0, false, 0.0, 0.0);
        } else {
            w.logs_area.set_visible(false);
        }
    }

    // Header status pill: daemon dot + Running/Stopped (click toggles enable/disable).
    let css = if snap.connected { "success" } else { "error" };
    for c in ["success", "error"] {
        w.daemon_dot.remove_css_class(c);
    }
    w.daemon_dot.add_css_class(css);
    w.daemon_label.set_label(if snap.connected { "Running" } else { "Stopped" });
    w.daemon_toggle.set_tooltip_text(Some(if snap.connected {
        "Daemon running — click to stop"
    } else {
        "Daemon stopped — click to start"
    }));

    // Docker-context selector (hidden when the docker CLI isn't installed).
    match &snap.docker_context {
        Some(active) => {
            w.context_seg.set_visible(true);
            w.context_menu.set_label(active);
            if active == "dd" {
                w.context_menu.add_css_class("dd-active");
            } else {
                w.context_menu.remove_css_class("dd-active");
            }
            w.context_menu.set_tooltip_text(Some("Choose which Docker context (daemon) the docker CLI uses"));
            // Rebuild the popover list of available contexts.
            clear_box(&w.context_pop_box);
            for ctx in &snap.docker_contexts {
                let lbl = gtk::Label::new(Some(ctx));
                lbl.set_xalign(0.0);
                lbl.set_hexpand(true);
                let item = gtk::Button::builder().child(&lbl).build();
                item.add_css_class("dd-popitem");
                item.set_has_frame(false);
                if ctx == active {
                    item.add_css_class("dd-active");
                }
                let s = sender.clone();
                let name = ctx.clone();
                let menu = w.context_menu.clone();
                item.connect_clicked(move |_| {
                    s.input(Msg::SetContext(name.clone()));
                    menu.popdown();
                });
                w.context_pop_box.append(&item);
            }
        }
        None => w.context_seg.set_visible(false),
    }
}

// ---- home dashboard --------------------------------------------------------

fn render_home(home: &gtk::Box, m: &AppModel, sender: &ComponentSender<AppModel>) {
    clear_box(home);
    let snap = &m.snap;

    let title = gtk::Label::new(Some("Overview"));
    title.set_xalign(0.0);
    title.add_css_class("dd-h1");
    home.append(&title);

    let running = snap.containers.iter().filter(|c| c.running()).count();
    let stats = gtk::Box::new(gtk::Orientation::Horizontal, 12);
    stats.append(&stat_card(&running.to_string(), "Running", true));
    stats.append(&stat_card(&snap.containers.len().to_string(), "Containers", false));
    stats.append(&stat_card(&snap.images.len().to_string(), "Images", false));
    home.append(&stats);

    if let Some(rel) = &m.update {
        home.append(&update_card(&rel.version, sender));
    }

    // ---- Get started -------------------------------------------------------
    let gs = gtk::Label::new(Some("Get started"));
    gs.set_xalign(0.0);
    gs.add_css_class("dd-h2");
    home.append(&gs);

    let card = gtk::Box::new(gtk::Orientation::Vertical, 12);
    card.add_css_class("dd-step-card");

    // 1. Run hello-world — works immediately (the image is bundled + seeded into ~/.dd/images).
    card.append(&action_row(
        "Run your first container",
        "A hello-world image is bundled — give it a try.",
        "Run hello-world",
        true,
        sender,
        || Msg::RunImage("hello-dd".to_string()),
    ));

    card.append(&gtk::Separator::new(gtk::Orientation::Horizontal));

    // 2. Terminal path: install the CLI, point Docker at dd, then the (working) command.
    card.append(&action_row(
        "Use the terminal",
        "Install the dd CLI, point Docker at dd (selector, top-right), then:",
        "Install CLI",
        false,
        sender,
        || Msg::InstallCli,
    ));
    let code = gtk::Label::new(Some("docker run --rm hello-dd"));
    code.set_xalign(0.0);
    code.set_hexpand(true); // full-width code box (not shrink-wrapped to the text)
    code.set_selectable(true);
    code.add_css_class("dd-code");
    card.append(&code);

    home.append(&card);
}

/// The Settings page: version, locations, CLI install, and the reset (danger) action.
fn render_settings(s: &gtk::Box, m: &AppModel, sender: &ComponentSender<AppModel>) {
    clear_box(s);

    let title = gtk::Label::new(Some("Settings"));
    title.set_xalign(0.0);
    title.add_css_class("dd-h1");
    s.append(&title);

    // About + locations.
    let home = std::env::var("HOME").unwrap_or_default();
    let about = setting_card(&[
        ("Version", env!("DD_VERSION")),
        ("Socket", &m.socket.to_string_lossy()),
        ("Images", &format!("{home}/.dd/images")),
        ("State", &format!("{home}/.dd/state.json")),
    ]);
    s.append(&about);

    // Command-line tool.
    let cli = gtk::Label::new(Some("Command-line tool"));
    cli.set_xalign(0.0);
    cli.add_css_class("dd-h2");
    s.append(&cli);
    let cli_card = gtk::Box::new(gtk::Orientation::Vertical, 0);
    cli_card.add_css_class("dd-step-card");
    cli_card.append(&action_row(
        "Install the dd CLI",
        "Adds dd to your terminal (~/.local/bin).",
        "Install",
        false,
        sender,
        || Msg::InstallCli,
    ));
    s.append(&cli_card);

    // Reset (danger).
    let rz = gtk::Label::new(Some("Reset"));
    rz.set_xalign(0.0);
    rz.add_css_class("dd-h2");
    s.append(&rz);
    let rt = gtk::Label::new(Some("Reset dd"));
    rt.set_xalign(0.0);
    rt.add_css_class("heading");
    let rd = gtk::Label::new(Some("Remove all containers, volumes and networks (images are kept)."));
    rd.set_xalign(0.0);
    rd.set_wrap(true);
    rd.add_css_class("dim-label");
    rd.add_css_class("caption");
    let rtexts = gtk::Box::new(gtk::Orientation::Vertical, 1);
    rtexts.set_hexpand(true);
    rtexts.set_valign(gtk::Align::Center);
    rtexts.append(&rt);
    rtexts.append(&rd);
    let rbtn = gtk::Button::with_label("Reset…");
    rbtn.add_css_class("dd-btn");
    rbtn.add_css_class("destructive-action");
    rbtn.set_valign(gtk::Align::Center);
    {
        let s2 = sender.clone();
        rbtn.connect_clicked(move |_| s2.input(Msg::ConfirmReset));
    }
    let rcard = gtk::Box::new(gtk::Orientation::Horizontal, 12);
    rcard.add_css_class("dd-step-card");
    rcard.append(&rtexts);
    rcard.append(&rbtn);
    s.append(&rcard);
}

/// A card of key/value rows (selectable monospace values) for the Settings page.
fn setting_card(rows: &[(&str, &str)]) -> gtk::Box {
    let card = gtk::Box::new(gtk::Orientation::Vertical, 8);
    card.add_css_class("dd-step-card");
    for (k, v) in rows {
        let key = gtk::Label::new(Some(k));
        key.set_xalign(0.0);
        key.set_width_request(72);
        key.add_css_class("dim-label");
        key.add_css_class("caption");
        let val = gtk::Label::new(Some(v));
        val.set_xalign(0.0);
        val.set_hexpand(true);
        val.set_selectable(true);
        val.set_ellipsize(gtk::pango::EllipsizeMode::Middle);
        val.add_css_class("dd-mono");
        let row = gtk::Box::new(gtk::Orientation::Horizontal, 10);
        row.append(&key);
        row.append(&val);
        card.append(&row);
    }
    card
}

fn stat_card(value: &str, name: &str, accent: bool) -> gtk::Widget {
    let v = gtk::Label::new(Some(value));
    v.set_xalign(0.0);
    v.add_css_class("dd-stat-value");
    if accent {
        v.add_css_class("accent");
    }
    let n = gtk::Label::new(Some(name));
    n.set_xalign(0.0);
    n.add_css_class("dd-stat-name");
    let c = gtk::Box::new(gtk::Orientation::Vertical, 2);
    c.add_css_class("dd-stat-card");
    c.set_size_request(132, -1);
    c.append(&v);
    c.append(&n);
    c.upcast()
}

fn update_card(version: &str, sender: &ComponentSender<AppModel>) -> gtk::Widget {
    let t = gtk::Label::new(Some(&format!("Update available — v{version}")));
    t.set_xalign(0.0);
    t.add_css_class("heading");
    let d = gtk::Label::new(Some("A newer version of dd is ready to install."));
    d.set_xalign(0.0);
    d.add_css_class("dim-label");
    let texts = gtk::Box::new(gtk::Orientation::Vertical, 1);
    texts.set_hexpand(true);
    texts.set_valign(gtk::Align::Center);
    texts.append(&t);
    texts.append(&d);
    let btn = gtk::Button::with_label("Install update");
    btn.add_css_class("dd-btn");
    btn.add_css_class("suggested-action");
    btn.set_valign(gtk::Align::Center);
    {
        let s = sender.clone();
        btn.connect_clicked(move |_| s.input(Msg::ApplyUpdate));
    }
    let row = gtk::Box::new(gtk::Orientation::Horizontal, 12);
    row.add_css_class("dd-update-card");
    row.append(&texts);
    row.append(&btn);
    row.upcast()
}

/// A "title + description on the left, action button on the right" row for the Get Started card.
fn action_row(
    title: &str,
    desc: &str,
    btn_label: &str,
    primary: bool,
    sender: &ComponentSender<AppModel>,
    msg: impl Fn() -> Msg + 'static,
) -> gtk::Box {
    let t = gtk::Label::new(Some(title));
    t.set_xalign(0.0);
    t.add_css_class("heading");
    let d = gtk::Label::new(Some(desc));
    d.set_xalign(0.0);
    d.set_wrap(true);
    d.add_css_class("dim-label");
    d.add_css_class("caption");
    let texts = gtk::Box::new(gtk::Orientation::Vertical, 1);
    texts.set_hexpand(true);
    texts.set_valign(gtk::Align::Center);
    texts.append(&t);
    texts.append(&d);
    let btn = gtk::Button::with_label(btn_label);
    btn.add_css_class("dd-btn");
    if primary {
        btn.add_css_class("suggested-action");
    }
    btn.set_valign(gtk::Align::Center);
    {
        let s = sender.clone();
        btn.connect_clicked(move |_| s.input(msg()));
    }
    let row = gtk::Box::new(gtk::Orientation::Horizontal, 12);
    row.append(&texts);
    row.append(&btn);
    row
}

/// A category nav row: symbolic icon + label.
fn cat_row(icon: &str, label: &str) -> gtk::ListBoxRow {
    let img = gtk::Image::from_icon_name(icon);
    img.set_pixel_size(16);
    let lbl = gtk::Label::new(Some(label));
    lbl.set_xalign(0.0);
    let b = gtk::Box::new(gtk::Orientation::Horizontal, 10);
    b.set_margin_top(7);
    b.set_margin_bottom(7);
    b.set_margin_start(4);
    b.append(&img);
    b.append(&lbl);
    let row = gtk::ListBoxRow::new();
    row.set_child(Some(&b));
    row
}

/// A container list row: short id on top, "image · state" below, with a running/stopped dot.
fn container_list_row(c: &Container) -> gtk::ListBoxRow {
    let dot = gtk::Box::new(gtk::Orientation::Horizontal, 0);
    dot.add_css_class("dd-dot");
    dot.add_css_class(if c.running() { "success" } else { "error" });
    dot.set_valign(gtk::Align::Center);

    let id = gtk::Label::new(Some(&c.short_id()));
    id.set_xalign(0.0);
    id.add_css_class("heading");
    let state = if c.running() { "running".to_string() } else { c.display_status() };
    let sub = gtk::Label::new(Some(&format!("{} · {}", c.image, state)));
    sub.set_xalign(0.0);
    sub.set_ellipsize(gtk::pango::EllipsizeMode::End);
    sub.add_css_class("caption");
    sub.add_css_class("dim-label");
    let v = gtk::Box::new(gtk::Orientation::Vertical, 1);
    v.set_hexpand(true);
    v.append(&id);
    v.append(&sub);

    let h = gtk::Box::new(gtk::Orientation::Horizontal, 10);
    h.set_margin_top(5);
    h.set_margin_bottom(5);
    h.set_margin_start(8);
    h.set_margin_end(8);
    h.append(&dot);
    h.append(&v);
    let row = gtk::ListBoxRow::new();
    row.set_child(Some(&h));
    row
}

/// Install the `dd` CLI and show a small window with a shell picker + matching PATH instructions.
pub fn show_cli_install(parent: &gtk::ApplicationWindow) {
    let result = crate::install_cli();
    let ok = result.is_ok();
    let on_path = result.as_ref().map(|(_, p)| *p).unwrap_or(false);
    let cmd = result
        .as_ref()
        .ok()
        .and_then(|(link, _)| link.file_name().map(|n| n.to_string_lossy().into_owned()))
        .unwrap_or_else(|| "dd".to_string());
    let status_text = match &result {
        Ok((link, _)) => format!("Installed to {}", link.display()),
        Err(e) => format!("Couldn't install: {e}"),
    };

    let heading = gtk::Label::new(Some("dd command-line tool"));
    heading.set_xalign(0.0);
    heading.add_css_class("dd-onboard-head");
    let status = gtk::Label::new(Some(&status_text));
    status.set_xalign(0.0);
    status.set_wrap(true);
    status.add_css_class("dd-sub");

    // Shell picker.
    let dropdown = gtk::DropDown::from_strings(&["zsh", "bash", "fish"]);
    dropdown.set_selected(detect_shell_index());
    let shell_lbl = gtk::Label::new(Some("Shell"));
    shell_lbl.add_css_class("dim-label");
    let shell_row = gtk::Box::new(gtk::Orientation::Horizontal, 10);
    shell_row.append(&shell_lbl);
    shell_row.append(&dropdown);
    shell_row.set_visible(ok && !on_path);

    // Per-shell instructions.
    let instr = gtk::Label::new(None);
    instr.set_xalign(0.0);
    instr.set_wrap(true);
    instr.set_selectable(true);
    instr.add_css_class("dd-cli-msg");
    instr.set_visible(ok);
    if ok {
        instr.set_label(&shell_instr(dropdown.selected(), on_path, &cmd));
        let instr2 = instr.clone();
        let cmd2 = cmd.clone();
        dropdown.connect_selected_notify(move |d| instr2.set_label(&shell_instr(d.selected(), on_path, &cmd2)));
    }

    let done = gtk::Button::with_label("Done");
    done.add_css_class("dd-btn");
    done.add_css_class("suggested-action");
    let btnrow = gtk::Box::new(gtk::Orientation::Horizontal, 0);
    btnrow.set_halign(gtk::Align::End);
    btnrow.set_margin_top(8);
    btnrow.append(&done);

    let v = gtk::Box::new(gtk::Orientation::Vertical, 10);
    v.set_margin_top(20);
    v.set_margin_bottom(18);
    v.set_margin_start(22);
    v.set_margin_end(22);
    v.append(&heading);
    v.append(&status);
    v.append(&shell_row);
    v.append(&instr);
    v.append(&btnrow);

    let win = gtk::Window::builder().title("Install dd CLI").modal(true).resizable(false).default_width(460).child(&v).build();
    win.set_transient_for(Some(parent));
    let w = win.clone();
    done.connect_clicked(move |_| w.close());
    win.present();
}

fn detect_shell_index() -> u32 {
    let sh = std::env::var("SHELL").unwrap_or_default();
    if sh.contains("fish") {
        2
    } else if sh.contains("bash") {
        1
    } else {
        0 // zsh (macOS default) or unknown
    }
}

fn shell_instr(idx: u32, on_path: bool, cmd: &str) -> String {
    if on_path {
        return format!("~/.local/bin is already on your PATH.\nJust run:  {cmd}");
    }
    match idx {
        1 => "Add to ~/.bashrc, then restart your terminal:\n\nexport PATH=\"$HOME/.local/bin:$PATH\"",
        2 => "Run once (fish):\n\nfish_add_path ~/.local/bin",
        _ => "Add to ~/.zshrc, then restart your terminal:\n\nexport PATH=\"$HOME/.local/bin:$PATH\"",
    }
    .to_string()
}

/// Confirm a full reset (remove all containers/volumes/networks). Frameless dialog, pill buttons.
pub fn confirm_reset(parent: &gtk::ApplicationWindow, sender: &ComponentSender<AppModel>) {
    let title = gtk::Label::new(Some("Reset dd?"));
    title.set_xalign(0.0);
    title.add_css_class("dd-onboard-head");
    let detail = gtk::Label::new(Some("This removes all containers, volumes and networks. Your images are kept."));
    detail.set_xalign(0.0);
    detail.set_wrap(true);
    detail.set_max_width_chars(36);
    detail.add_css_class("dim-label");

    let cancel = gtk::Button::with_label("Cancel");
    cancel.add_css_class("dd-btn");
    let ok = gtk::Button::with_label("Reset");
    ok.add_css_class("dd-btn");
    ok.add_css_class("destructive-action");
    let btns = gtk::Box::new(gtk::Orientation::Horizontal, 8);
    btns.set_halign(gtk::Align::End);
    btns.set_margin_top(8);
    btns.append(&cancel);
    btns.append(&ok);

    let v = gtk::Box::new(gtk::Orientation::Vertical, 8);
    v.add_css_class("dd-dialog");
    v.set_margin_top(20);
    v.set_margin_bottom(18);
    v.set_margin_start(22);
    v.set_margin_end(22);
    v.append(&title);
    v.append(&detail);
    v.append(&btns);

    let win = gtk::Window::builder().modal(true).resizable(false).decorated(false).child(&v).build();
    win.set_transient_for(Some(parent));
    let w1 = win.clone();
    cancel.connect_clicked(move |_| w1.close());
    let s = sender.clone();
    let w2 = win.clone();
    ok.connect_clicked(move |_| {
        s.input(Msg::Reset);
        w2.close();
    });
    win.present();
}

/// On first launch, offer to point the `docker` CLI at our daemon (the `dd` context). A small,
/// frameless dialog using the app's own pill buttons.
pub fn prompt_switch_context(parent: &gtk::ApplicationWindow, sender: &ComponentSender<AppModel>, current: &str) {
    let title = gtk::Label::new(Some("Use dd as your Docker context?"));
    title.set_xalign(0.0);
    title.add_css_class("title-3");
    let detail = gtk::Label::new(Some(&format!("Point the docker CLI at this app (switch from \u{201c}{current}\u{201d} to \u{201c}dd\u{201d}).")));
    detail.set_xalign(0.0);
    detail.set_wrap(true);
    detail.set_max_width_chars(34);
    detail.add_css_class("dim-label");

    let cancel = gtk::Button::with_label("Not now");
    cancel.add_css_class("dd-btn");
    let ok = gtk::Button::with_label("Switch to dd");
    ok.add_css_class("dd-btn");
    ok.add_css_class("suggested-action");
    let btns = gtk::Box::new(gtk::Orientation::Horizontal, 8);
    btns.set_halign(gtk::Align::End);
    btns.set_margin_top(8);
    btns.append(&cancel);
    btns.append(&ok);

    let v = gtk::Box::new(gtk::Orientation::Vertical, 8);
    v.add_css_class("dd-dialog");
    v.set_margin_top(20);
    v.set_margin_bottom(18);
    v.set_margin_start(22);
    v.set_margin_end(22);
    v.append(&title);
    v.append(&detail);
    v.append(&btns);

    let win = gtk::Window::builder().modal(true).resizable(false).decorated(false).child(&v).build();
    win.set_transient_for(Some(parent));

    let w1 = win.clone();
    cancel.connect_clicked(move |_| w1.close());
    let s = sender.clone();
    let w2 = win.clone();
    ok.connect_clicked(move |_| {
        s.input(Msg::SetContext("dd".to_string()));
        w2.close();
    });
    win.present();
}

// ---- detail builders -------------------------------------------------------

fn container_info(c: &Container, networks: &[Network], sender: &ComponentSender<AppModel>) -> gtk::Widget {
    let root = detail_root();

    // Header: title + actions.
    let mut actions = Vec::new();
    if !c.running() {
        actions.push(text_btn("Start", "suggested-action", sender, {
            let id = c.id.clone();
            move || Msg::StartContainer(id.clone())
        }));
    }
    actions.push(text_btn("Delete", "destructive-action", sender, {
        let id = c.id.clone();
        move || Msg::RemoveContainer(id.clone())
    }));
    let status = if c.running() {
        "running".to_string()
    } else {
        format!("{} (exit {})", c.display_status(), c.exit_code)
    };
    root.append(&detail_header(&c.name(), &format!("{} · {}", c.image, status), actions));

    // Sections.
    let mounts: Vec<String> = c.mounts.iter().map(|m| format!("{}  →  {}", m.source, m.destination)).collect();
    let nets: Vec<String> = networks
        .iter()
        .filter(|n| n.containers.keys().any(|cid| cid == &c.id))
        .map(|n| n.name.clone())
        .collect();
    let ports = c.ports_str();

    root.append(&section("Ports", &if ports.is_empty() { vec![] } else { vec![ports] }));
    root.append(&section("Volumes", &mounts));
    root.append(&section("Networks", &nets));
    root.upcast()
}

fn image_detail(img: &Image, sender: &ComponentSender<AppModel>) -> gtk::Widget {
    let root = detail_root();
    let run = text_btn("Run", "suggested-action", sender, {
        let name = img.name();
        move || Msg::RunImage(name.clone())
    });
    let del = text_btn("Delete", "destructive-action", sender, {
        let name = img.name();
        move || Msg::RemoveImage(name.clone())
    });
    let arch = if img.architecture.is_empty() { "unknown".into() } else { img.architecture.clone() };
    root.append(&detail_header(&img.name(), &arch, vec![run, del]));
    root.append(&section("Size", &[human_size(img.size)]));
    root.upcast()
}

/// Format a byte count compactly (B / KB / MB / GB).
fn human_size(bytes: i64) -> String {
    let b = bytes.max(0) as f64;
    if b < 1024.0 {
        format!("{} B", bytes.max(0))
    } else if b < 1024.0 * 1024.0 {
        format!("{:.0} KB", b / 1024.0)
    } else if b < 1024.0 * 1024.0 * 1024.0 {
        format!("{:.1} MB", b / (1024.0 * 1024.0))
    } else {
        format!("{:.1} GB", b / (1024.0 * 1024.0 * 1024.0))
    }
}

fn detail_root() -> gtk::Box {
    let b = gtk::Box::new(gtk::Orientation::Vertical, 18);
    b.set_margin_top(22);
    b.set_margin_bottom(22);
    b.set_margin_start(24);
    b.set_margin_end(24);
    b
}

fn detail_header(title: &str, subtitle: &str, actions: Vec<gtk::Button>) -> gtk::Widget {
    let titles = gtk::Box::new(gtk::Orientation::Vertical, 2);
    titles.set_hexpand(true);
    titles.set_valign(gtk::Align::Center);
    let t = gtk::Label::new(Some(title));
    t.set_xalign(0.0);
    t.add_css_class("title-2");
    let s = gtk::Label::new(Some(subtitle));
    s.set_xalign(0.0);
    s.set_wrap(true);
    s.add_css_class("dim-label");
    titles.append(&t);
    titles.append(&s);

    let row = gtk::Box::new(gtk::Orientation::Horizontal, 8);
    row.append(&titles);
    for b in actions {
        b.set_valign(gtk::Align::Center);
        row.append(&b);
    }
    row.upcast()
}

/// A titled section: a caption header and either its value rows or a dim em-dash.
fn section(title: &str, lines: &[String]) -> gtk::Widget {
    let b = gtk::Box::new(gtk::Orientation::Vertical, 4);
    let cap = gtk::Label::new(Some(&title.to_uppercase()));
    cap.set_xalign(0.0);
    cap.add_css_class("dd-section-title");
    b.append(&cap);

    if lines.is_empty() {
        let l = gtk::Label::new(Some("—"));
        l.set_xalign(0.0);
        l.add_css_class("dim-label");
        b.append(&l);
    } else {
        for line in lines {
            let l = gtk::Label::new(Some(line));
            l.set_xalign(0.0);
            l.set_wrap(true);
            l.set_selectable(true);
            b.append(&l);
        }
    }
    b.upcast()
}

// ---- sidebar helpers -------------------------------------------------------

fn nav_list() -> gtk::ListBox {
    let l = gtk::ListBox::new();
    l.set_selection_mode(gtk::SelectionMode::Single);
    l.add_css_class("navigation-sidebar");
    l
}

fn nav_item(title: &str, subtitle: &str, running: bool) -> gtk::ListBoxRow {
    let v = gtk::Box::new(gtk::Orientation::Vertical, 1);
    v.set_margin_top(4);
    v.set_margin_bottom(4);
    let t = gtk::Label::new(Some(title));
    t.set_xalign(0.0);
    t.set_ellipsize(gtk::pango::EllipsizeMode::End);
    v.append(&t);
    if !subtitle.is_empty() {
        let s = gtk::Label::new(Some(subtitle));
        s.set_xalign(0.0);
        s.add_css_class("caption");
        s.add_css_class(if running { "success" } else { "dim-label" });
        v.append(&s);
    }
    let row = gtk::ListBoxRow::new();
    row.set_child(Some(&v));
    row
}

fn section_caption(text: &str) -> gtk::Label {
    let l = gtk::Label::new(Some(&text.to_uppercase()));
    l.set_xalign(0.0);
    l.add_css_class("dd-section-title");
    l.set_margin_top(10);
    l.set_margin_bottom(2);
    l.set_margin_start(12);
    l
}

fn dim_row(text: &str) -> gtk::ListBoxRow {
    let l = gtk::Label::new(Some(text));
    l.set_xalign(0.0);
    l.set_margin_top(6);
    l.set_margin_bottom(6);
    l.set_margin_start(8);
    l.add_css_class("dim-label");
    let row = gtk::ListBoxRow::new();
    row.set_selectable(false);
    row.set_activatable(false);
    row.set_child(Some(&l));
    row
}

// ---- small helpers ---------------------------------------------------------

fn text_btn(
    label: &str,
    css: &str,
    sender: &ComponentSender<AppModel>,
    msg: impl Fn() -> Msg + 'static,
) -> gtk::Button {
    let b = gtk::Button::with_label(label);
    b.add_css_class("dd-btn");
    if !css.is_empty() {
        b.add_css_class(css);
    }
    let s = sender.clone();
    b.connect_clicked(move |_| s.input(msg()));
    b
}

fn placeholder(text: &str) -> gtk::Widget {
    let l = gtk::Label::new(Some(text));
    l.add_css_class("dim-label");
    l.set_vexpand(true);
    l.set_hexpand(true);
    l.set_valign(gtk::Align::Center);
    l.set_halign(gtk::Align::Center);
    l.upcast()
}

fn select_named(list: &gtk::ListBox, name: &str) {
    let mut i = 0;
    while let Some(row) = list.row_at_index(i) {
        if row.widget_name().as_str() == name {
            list.select_row(Some(&row));
            return;
        }
        i += 1;
    }
}

fn clear(list: &gtk::ListBox) {
    while let Some(child) = list.first_child() {
        list.remove(&child);
    }
}

fn clear_box(b: &gtk::Box) {
    while let Some(child) = b.first_child() {
        b.remove(&child);
    }
}

// ---- styling ---------------------------------------------------------------

/// Flat, simple, macOS-leaning CSS: gray window, a single floating base-color pane inset with a
/// border radius ("pane in pane"), faintly tinted sidebar, no gradients. Uses theme color names so
/// light/dark both work.
const CSS: &str = "
/* Use the macOS system font and a lighter, less-gray window. */
window {
  background-color: mix(@theme_bg_color, @theme_base_color, 0.55);
  font-family: 'SF Pro Text', 'Helvetica Neue', sans-serif;
  font-size: 13px;
}
.dd-topstrip { background: transparent; }

/* The content 'main frame': a solid floating card. */
.dd-content {
  background-color: @theme_base_color;
  border: 1px solid alpha(@borders, 0.7);
  border-radius: 12px;
}
/* The sidebar: a separate, faintly translucent floating card (its own surface). */
.dd-sidebar {
  background-color: alpha(@theme_base_color, 0.5);
  border: 1px solid alpha(@borders, 0.55);
  border-radius: 12px;
}

/* The paned handle is just the gap between the two cards — no border/grip; matches the nav gap. */
paned > separator {
  background-color: transparent;
  background-image: none;
  border: none;
  box-shadow: none;
  min-width: 7px;
}

/* Sidebar nav: rounded selection 'pills' like macOS source lists. */
list.navigation-sidebar { background: transparent; padding: 4px 6px; }
list.navigation-sidebar > row { border-radius: 7px; margin: 1px 2px; }

/* Coupled header status (daemon | docker): flat clickable text, no pill/box chrome. */
.dd-statusgroup { background: none; border: none; padding: 0; }
.dd-seg {
  background: none;
  background-color: transparent;
  border: none;
  box-shadow: none;
  outline: none;
  border-radius: 6px;
  padding: 2px 9px;
  min-height: 0;
  font-weight: 500;
}
button.dd-seg:hover { background-color: alpha(@theme_fg_color, 0.06); }
.dd-seg.dd-active { color: #1a8f3c; font-weight: 700; }
/* MenuButton selector: keep the outer node bare; only the inner button shows hover (no overlap). */
menubutton.dd-seg, menubutton.dd-seg > button {
  background: none; background-color: transparent; border: none; box-shadow: none; outline: none; min-height: 0;
}
menubutton.dd-seg > button { border-radius: 6px; padding: 3px 8px; font-weight: 500; }
menubutton.dd-seg > button:hover { background-color: alpha(@theme_fg_color, 0.06); }

/* Status circle, native macOS colors. */
.dd-dot {
  min-width: 9px; min-height: 9px;
  border-radius: 50%;
  background-color: alpha(@theme_fg_color, 0.35);
}
.dd-dot.success { background-color: #30d158; }
.dd-dot.error { background-color: #ff453a; }

/* App pill buttons (detail actions + dialog): capsule, subtle neutral fill, frameless. */
button { box-shadow: none; }
button.flat { padding: 4px 7px; border-radius: 7px; }
.dd-btn {
  padding: 5px 16px;
  min-height: 0;
  font-weight: 500;
  border: none;
  box-shadow: none;
  border-radius: 9999px;
  background-color: alpha(@theme_fg_color, 0.08);
}
.dd-btn:hover { background-color: alpha(@theme_fg_color, 0.13); }
.dd-btn.suggested-action { background-color: #0a84ff; color: #ffffff; }
.dd-btn.suggested-action:hover { background-color: #2b95ff; }
/* Delete: solid red destructive button with white text. */
.dd-btn.destructive-action { background-color: #ff453a; color: #ffffff; }
.dd-btn.destructive-action:hover { background-color: #ff5b51; }

/* Docker context selector popover: a clean, simple menu. */
popover.dd-pop { background: transparent; }
popover.dd-pop > arrow { background: transparent; border: none; }
popover.dd-pop > contents {
  padding: 4px;
  border-radius: 10px;
  background-color: @theme_base_color;
  border: none;
}
.dd-popitem {
  background: none; background-color: transparent;
  border: none; box-shadow: none; outline: none; min-height: 0;
  border-radius: 6px; padding: 5px 10px; font-weight: 400;
}
.dd-popitem:hover { background-color: alpha(@theme_fg_color, 0.07); }
.dd-popitem.dd-active { color: #0a84ff; font-weight: 600; }

.dd-dialog { background-color: @theme_base_color; border-radius: 14px; }

/* 'Update available' pill in the top strip. */
.dd-update { background-color: #0a84ff; color: #ffffff; border: none; box-shadow: none; border-radius: 7px; padding: 3px 11px; font-weight: 600; min-height: 0; }
.dd-update:hover { background-color: #2b95ff; }

/* Nav icons. */
list.navigation-sidebar image { opacity: 0.7; }

/* Home dashboard. */
.dd-home { background: transparent; }
.dd-h1 { font-size: 22px; font-weight: 800; margin-bottom: 2px; }
.dd-stat-card {
  background-color: @theme_base_color;
  border: 1px solid alpha(@borders, 0.7);
  border-radius: 12px;
  padding: 14px 16px;
}
.dd-stat-value { font-size: 28px; font-weight: 800; }
.dd-stat-value.accent { color: #30a14e; }
.dd-stat-name { font-size: 12px; opacity: 0.55; }
.dd-update-card {
  background-color: alpha(#0a84ff, 0.10);
  border: 1px solid alpha(#0a84ff, 0.30);
  border-radius: 12px;
  padding: 14px 16px;
}
.dd-h2 { font-size: 15px; font-weight: 700; margin-top: 6px; }
.dd-mono { font-family: 'SF Mono', 'Menlo', monospace; font-size: 12px; }
.dd-step-card {
  background-color: @theme_base_color;
  border: 1px solid alpha(@borders, 0.7);
  border-radius: 12px;
  padding: 11px 16px;
}
.dd-code {
  font-family: 'SF Mono', 'Menlo', monospace;
  font-size: 12px;
  color: alpha(@theme_fg_color, 0.85);
  background-color: alpha(@theme_fg_color, 0.06);
  border: 1px solid alpha(@borders, 0.6);
  border-radius: 8px;
  padding: 10px 12px;
}

/* Onboarding / splash screen — slightly larger, clearer text (not overdone). */
.dd-bigtitle { font-size: 34px; font-weight: 800; }
.dd-sub { font-size: 13.5px; opacity: 0.6; }
.dd-onboard-status { font-size: 13.5px; font-weight: 600; }
.dd-onboard-head { font-size: 15px; font-weight: 700; }
.dd-cli-msg {
  font-family: 'SF Mono', 'Menlo', monospace;
  font-size: 11px;
  background-color: alpha(@theme_fg_color, 0.05);
  border-radius: 8px;
  padding: 8px 10px;
  margin-top: 6px;
}

/* Inline logs pane at the bottom of the detail card. */
.dd-logs, .dd-logs text {
  background-color: alpha(@theme_fg_color, 0.04);
  font-family: 'SF Mono', 'Menlo', monospace;
  font-size: 12px;
}

.dd-section-title { font-size: 0.72em; font-weight: 700; opacity: 0.5; letter-spacing: 0.04em; }
";

fn load_css() {
    let provider = gtk::CssProvider::new();
    provider.load_from_data(CSS);
    if let Some(display) = gtk::gdk::Display::default() {
        gtk::style_context_add_provider_for_display(
            &display,
            &provider,
            gtk::STYLE_PROVIDER_PRIORITY_APPLICATION,
        );
    }
}

// ---- bundle environment ----------------------------------------------------

/// When running from inside `dd-app.app`, point GTK at the bundled runtime data. No-op for a dev
/// build (the Resources/Frameworks dirs won't exist), and never overrides an env var already set.
pub fn setup_bundle_env() {
    let Ok(exe) = std::env::current_exe() else { return };
    let Some(contents) = exe.parent().and_then(|p| p.parent()) else { return };
    let res = contents.join("Resources");
    let fw = contents.join("Frameworks");
    if !res.exists() || !fw.exists() {
        return; // not a bundle — dev run
    }
    let loaders = res.join("lib/gdk-pixbuf-2.0/2.10.0/loaders");
    set_if_absent("GSETTINGS_SCHEMA_DIR", res.join("glib-2.0/schemas").as_os_str());
    set_if_absent("GSETTINGS_BACKEND", OsStr::new("memory"));
    set_if_absent("GDK_PIXBUF_MODULE_FILE", loaders.join("loaders.cache").as_os_str());
    set_if_absent("GDK_PIXBUF_MODULEDIR", loaders.as_os_str());
    set_if_absent("XDG_DATA_DIRS", res.as_os_str());
    set_if_absent("XDG_DATA_HOME", res.as_os_str());
    set_if_absent("GTK_PATH", fw.as_os_str());
    set_if_absent("FONTCONFIG_FILE", res.join("fontconfig/fonts.conf").as_os_str());
    set_if_absent("GSK_RENDERER", OsStr::new("gl"));
}

fn set_if_absent(key: &str, val: &OsStr) {
    if std::env::var_os(key).is_none() {
        std::env::set_var(key, val);
    }
}


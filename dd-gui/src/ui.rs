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
    onboard_status: gtk::Label,
    cli_label: gtk::Label,
    nav: gtk::ListBox,
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

    // --- pane 1: category nav (Containers / Images) ------------------------
    let nav = nav_list();
    nav.append(&nav_item("Containers", "", false));
    nav.append(&nav_item("Images", "", false));
    {
        let s = sender.clone();
        nav.connect_row_activated(move |_, row| {
            let cat = if row.index() == 0 { Category::Containers } else { Category::Images };
            s.input(Msg::SetCategory(cat));
        });
    }
    let nav_card = sidebar_card();
    nav_card.set_size_request(148, -1);
    nav_card.append(&nav);

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
    let logs_cap = section_caption("Logs");
    logs_cap.set_margin_bottom(0);
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

    let body = gtk::Box::new(gtk::Orientation::Horizontal, 7);
    body.set_margin_top(0);
    body.set_margin_bottom(7);
    body.set_margin_start(7);
    body.set_margin_end(7);
    body.append(&nav_card);
    body.append(&paned);

    // Onboarding / splash screen, shown when the daemon is off.
    let (onboarding, onboard_status, cli_label) = build_onboarding(sender);

    // Two views: compact onboarding vs. the full app.
    let stack = gtk::Stack::new();
    stack.set_transition_type(gtk::StackTransitionType::Crossfade);
    stack.add_named(&onboarding, Some("onboarding"));
    stack.add_named(&body, Some("main"));

    // Slim status strip (under the native title bar) above the body.
    let strip = gtk::Box::new(gtk::Orientation::Horizontal, 0);
    strip.add_css_class("dd-topstrip");
    strip.set_size_request(-1, 38); // title-bar height; traffic lights float over the (empty) left
    strip.set_margin_start(12);
    strip.set_margin_end(7);
    strip.append(&group);

    let outer = gtk::Box::new(gtk::Orientation::Vertical, 0);
    outer.append(&strip);
    outer.append(&stack);
    root.set_child(Some(&outer));

    // Unify the title bar with our content (native traffic lights float over the strip).
    #[cfg(target_os = "macos")]
    root.connect_realize(|_| crate::mac::unify_titlebar());

    Widgets {
        stack, onboard_status, cli_label,
        nav, list, detail_info, logs_area, logs_view,
        daemon_dot, daemon_label, daemon_toggle, context_menu, context_pop_box, context_seg,
    }
}

/// The onboarding/splash view: a 2-column layout — branding + enable on the left, CLI install on
/// the right.
fn build_onboarding(sender: &ComponentSender<AppModel>) -> (gtk::Widget, gtk::Label, gtk::Label) {
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
    let cli_label = gtk::Label::new(None);
    cli_label.set_wrap(true);
    cli_label.set_xalign(0.0);
    cli_label.set_visible(false);
    cli_label.add_css_class("dd-cli-msg");
    let right = gtk::Box::new(gtk::Orientation::Vertical, 7);
    right.set_valign(gtk::Align::Center);
    right.set_size_request(250, -1);
    right.append(&cli_head);
    right.append(&cli_desc);
    right.append(&install);
    right.append(&cli_label);

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
    (outer.upcast(), onboard_status, cli_label)
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

    // Onboarding (compact) vs. the full app.
    w.stack.set_visible_child_name(if snap.connected { "main" } else { "onboarding" });
    w.onboard_status.set_label(if snap.connected {
        "The dd daemon is running."
    } else {
        "The dd daemon is not running."
    });
    match &m.cli_msg {
        Some(msg) => {
            w.cli_label.set_visible(true);
            w.cli_label.set_label(msg);
        }
        None => w.cli_label.set_visible(false),
    }

    // Pane 1: reflect the active category.
    let cat_idx = if m.category == Category::Images { 1 } else { 0 };
    w.nav.select_row(w.nav.row_at_index(cat_idx).as_ref());

    // Pane 2: the items in the active category.
    clear(&w.list);
    match m.category {
        Category::Containers => {
            if snap.containers.is_empty() {
                w.list.append(&dim_row(if snap.connected { "No containers" } else { "—" }));
            }
            for c in &snap.containers {
                let sub = if c.running() { "running".to_string() } else { c.display_status() };
                let row = nav_item(&c.name(), &sub, c.running());
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
    let arch = if img.architecture.is_empty() { "unknown".into() } else { img.architecture.clone() };
    root.append(&detail_header(&img.name(), &arch, vec![run]));
    root.append(&section("Size", &if img.size > 0 { vec![format!("{} bytes", img.size)] } else { vec![] }));
    root.upcast()
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


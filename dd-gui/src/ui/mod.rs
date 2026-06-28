//! dd-app UI: the window shell + render dispatch. Resource pages live in `views/`, reusable
//! widgets and dialogs in `components/`, the design tokens in `theme`.
#![allow(unused_imports, dead_code)]
pub(crate) mod theme;
pub(crate) mod components;
pub(crate) mod views;
pub(crate) use components::*;
pub(crate) use views::*;
pub(crate) use theme::*;
use crate::{AppModel, Category, Msg, Selection};
use ddclient::{Container, Image, Network, Volume};
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
    system_logs: gtk::Box,
    list: gtk::ListBox,
    list_sig: std::rc::Rc<std::cell::RefCell<String>>,
    batch: std::rc::Rc<std::cell::RefCell<std::collections::HashSet<String>>>,
    detail_info: gtk::Box,
    term_notebook: gtk::Notebook,
    term_current: crate::ui::components::TermTarget,
    term_shells: crate::ui::components::TermShells,
    logs_view: gtk::TextView,
    logs_scroll: gtk::ScrolledWindow,
    add_term: gtk::Widget,
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
    nav.append(&cat_row("network-workgroup-symbolic", "Networks"));
    nav.append(&cat_row("drive-harddisk-symbolic", "Volumes"));
    nav.append(&cat_row("emblem-system-symbolic", "System"));
    // Settings now lives inside the System page (its second tab), so the sidebar has no separate
    // Settings entry — `nav_bottom` is kept empty for layout symmetry.
    let nav_bottom = nav_list();
    {
        let s = sender.clone();
        nav.connect_row_activated(move |_, row| {
            let cat = match row.index() {
                1 => Category::Containers,
                2 => Category::Images,
                3 => Category::Networks,
                4 => Category::Volumes,
                5 => Category::System,
                _ => Category::Home,
            };
            s.input(Msg::SetCategory(cat));
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
    // Plain click = VIEW one item (Single selection + detail). BATCH selection is separate and ⌘-only:
    // ⌘-clicked rows go into `batch` (a `.dd-batch` highlight, NOT the view selection); Delete removes them.
    list.set_selection_mode(gtk::SelectionMode::Single);
    list.set_activate_on_single_click(true);
    let batch: std::rc::Rc<std::cell::RefCell<std::collections::HashSet<String>>> =
        std::rc::Rc::new(std::cell::RefCell::new(std::collections::HashSet::new()));
    {
        let s = sender.clone();
        list.connect_row_activated(move |_, row| {
            let wn = row.widget_name();
            if let Some(id) = wn.as_str().strip_prefix("c:") {
                s.input(Msg::Select(Selection::Container(id.to_string())));
            } else if let Some(name) = wn.as_str().strip_prefix("i:") {
                s.input(Msg::Select(Selection::Image(name.to_string())));
            } else if let Some(id) = wn.as_str().strip_prefix("n:") {
                s.input(Msg::Select(Selection::Network(id.to_string())));
            } else if let Some(name) = wn.as_str().strip_prefix("v:") {
                s.input(Msg::Select(Selection::Volume(name.to_string())));
            }
        });
    }
    {
        // Delete / Backspace → remove every ⌘-batched item (only the batch; plain-selected view is NOT
        // deleted by the keyboard — use the detail Delete button for that).
        let s = sender.clone();
        let batch_k = batch.clone();
        let keys = gtk::EventControllerKey::new();
        keys.connect_key_pressed(move |_, keyval, _kc, _mods| {
            if matches!(keyval, gtk::gdk::Key::Delete | gtk::gdk::Key::BackSpace) {
                let items: Vec<String> = batch_k.borrow().iter().cloned().collect();
                for n in items {
                    if let Some(id) = n.strip_prefix("c:") {
                        s.input(Msg::RemoveContainer(id.to_string()));
                    } else if let Some(x) = n.strip_prefix("i:") {
                        s.input(Msg::RemoveImage(x.to_string()));
                    } else if let Some(x) = n.strip_prefix("n:") {
                        s.input(Msg::RemoveNetwork(x.to_string()));
                    } else if let Some(x) = n.strip_prefix("v:") {
                        s.input(Msg::RemoveVolume(x.to_string()));
                    }
                }
                batch_k.borrow_mut().clear();
                return gtk::glib::Propagation::Stop;
            }
            gtk::glib::Propagation::Proceed
        });
        list.add_controller(keys);
    }
    {
        // ⌘-click toggles BATCH membership (a `.dd-batch` highlight), without changing the viewed item.
        // Capture phase + claim so the default single-select/activate doesn't also fire.
        let click = gtk::GestureClick::new();
        click.set_button(gtk::gdk::BUTTON_PRIMARY);
        click.set_propagation_phase(gtk::PropagationPhase::Capture);
        let list_ref = list.clone();
        let batch_c = batch.clone();
        click.connect_pressed(move |g, _n, _x, y| {
            let cmd = g.current_event()
                .map(|e| e.modifier_state().contains(gtk::gdk::ModifierType::META_MASK))
                .unwrap_or(false);
            if cmd {
                if let Some(row) = list_ref.row_at_y(y as i32) {
                    let key = row.widget_name().to_string();
                    if key.contains(':') {
                        let mut b = batch_c.borrow_mut();
                        if b.remove(&key) {
                            row.remove_css_class("dd-batch");
                        } else {
                            b.insert(key);
                            row.add_css_class("dd-batch");
                        }
                    }
                    g.set_state(gtk::EventSequenceState::Claimed);
                }
            }
        });
        list.add_controller(click);
    }
    // Only rebuilt when the item set changes (so the ⌘-batch highlight survives the 2s poll).
    let list_sig = std::rc::Rc::new(std::cell::RefCell::new(String::new()));
    let list_scroll = gtk::ScrolledWindow::builder()
        .child(&list)
        .hscrollbar_policy(gtk::PolicyType::Never)
        .vexpand(true)
        .build();
    let list_card = sidebar_card();
    list_card.append(&list_scroll);

    // --- pane 3: detail — ONE tabbed card: [Info] [Logs] + interactive terminal tabs (＋). Built once
    // and persisted so terminals + the selected tab survive the 2s state poll.
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
    let logs_scroll = gtk::ScrolledWindow::builder().child(&logs_view).vexpand(true).hexpand(true).build();

    let term_notebook = gtk::Notebook::new();
    term_notebook.add_css_class("dd-termbook");
    term_notebook.add_css_class("dd-content"); // the whole detail is one card
    term_notebook.set_overflow(gtk::Overflow::Hidden);
    term_notebook.set_scrollable(true);
    term_notebook.append_page(&info_scroll, Some(&gtk::Label::new(Some("Info"))));
    term_notebook.append_page(&logs_scroll, Some(&gtk::Label::new(Some("Logs"))));
    let term_current: crate::ui::components::TermTarget = std::rc::Rc::new(std::cell::RefCell::new(None));
    let term_shells: crate::ui::components::TermShells = std::rc::Rc::new(std::cell::RefCell::new(Vec::new()));
    let add_term = crate::ui::components::new_terminal_button(&term_notebook, term_current.clone(), term_shells.clone());
    term_notebook.set_action_widget(&add_term, gtk::PackType::End);

    let detail_col = gtk::Box::new(gtk::Orientation::Vertical, 0);
    detail_col.set_size_request(360, -1);
    detail_col.append(&term_notebook);

    // list | detail are resizable; nav stays fixed.
    let paned = gtk::Paned::new(gtk::Orientation::Horizontal);
    paned.set_start_child(Some(&list_card));
    paned.set_end_child(Some(&detail_col));
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

    // System page: a tabbed notebook — "Logs" (engine + disk + the daemon log, filled by render()) and
    // "Settings" (the settings box, also filled by render()).
    let system_logs = gtk::Box::new(gtk::Orientation::Vertical, 16);
    system_logs.set_margin_top(18);
    system_logs.set_margin_bottom(18);
    system_logs.set_margin_start(20);
    system_logs.set_margin_end(20);
    let system_logs_scroll = gtk::ScrolledWindow::builder()
        .child(&system_logs)
        .hscrollbar_policy(gtk::PolicyType::Never)
        .vexpand(true)
        .hexpand(true)
        .build();
    let system_notebook = gtk::Notebook::new();
    system_notebook.add_css_class("dd-termbook");
    system_notebook.add_css_class("dd-syspages");
    system_notebook.append_page(&system_logs_scroll, Some(&gtk::Label::new(Some("Logs"))));
    system_notebook.append_page(&settings_scroll, Some(&gtk::Label::new(Some("Settings"))));

    // Content area: dashboard ("home") / system (tabs) / browse (list | detail).
    let content_stack = gtk::Stack::new();
    content_stack.set_hexpand(true);
    content_stack.add_named(&home_scroll, Some("home"));
    content_stack.add_named(&system_notebook, Some("system"));
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
        nav, nav_bottom, content_stack, home, settings, system_logs, list, list_sig, batch, detail_info, term_notebook, term_current, term_shells, logs_view, logs_scroll, add_term,
        daemon_dot, daemon_label, daemon_toggle, context_menu, context_pop_box, context_seg,
    }
}


/// Locate the dd logo: the app bundle's `Contents/Resources/logo.png`, or `assets/logo.png` in dev.
pub(crate) fn logo_path() -> Option<std::path::PathBuf> {
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
pub(crate) fn sidebar_card() -> gtk::Box {
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

    // Pane 1: reflect the active category in the nav (Settings now lives inside the System page).
    let idx = match m.category {
        Category::Containers => 1,
        Category::Images => 2,
        Category::Networks => 3,
        Category::Volumes => 4,
        Category::System | Category::Settings => 5,
        _ => 0,
    };
    w.nav.select_row(w.nav.row_at_index(idx).as_ref());

    if m.category == Category::Home {
        w.content_stack.set_visible_child_name("home");
        render_home(&w.home, m, sender);
    } else if m.category == Category::System || m.category == Category::Settings {
        // System page = [Logs][Settings] tabs; fill both (the notebook keeps the selected tab).
        w.content_stack.set_visible_child_name("system");
        render_system(&w.system_logs, m, sender);
        render_settings(&w.settings, m, sender);
    } else {
        w.content_stack.set_visible_child_name("browse");

        // Pane 2: only REBUILD the master list when the item set actually changes — otherwise the 2s
        // poll would wipe the user's (multi-)selection. The signature keys on category + ids + state.
        let sig = match m.category {
            Category::Containers => snap.containers.iter().map(|c| format!("c{}:{}:{}", c.id, c.running(), m.removing.contains(&c.id))).collect::<Vec<_>>().join(","),
            Category::Images => snap.images.iter().map(|i| format!("i{}:{}", i.name(), i.size)).collect::<Vec<_>>().join(","),
            Category::Networks => snap.networks.iter().map(|n| format!("n{}", n.id)).collect::<Vec<_>>().join(","),
            Category::Volumes => snap.volumes.iter().map(|v| format!("v{}", v.name)).collect::<Vec<_>>().join(","),
            _ => String::new(),
        };
        let sig = format!("{:?}|{}|{sig}", m.category, snap.connected);
        if *w.list_sig.borrow() != sig {
            *w.list_sig.borrow_mut() = sig;
            clear(&w.list);
            match m.category {
                Category::Containers => {
                    if snap.containers.is_empty() {
                        w.list.append(&dim_row(if snap.connected { "No containers" } else { "—" }));
                    }
                    for c in &snap.containers {
                        let row = container_list_row(c, m.removing.contains(&c.id));
                        row.set_widget_name(&format!("c:{}", c.id));
                        w.list.append(&row);
                    }
                }
                Category::Images => {
                    if snap.images.is_empty() {
                        w.list.append(&dim_row("No images"));
                    }
                    for img in &snap.images {
                        let row = nav_item(&img.name(), &human_size(img.size), false);
                        row.set_widget_name(&format!("i:{}", img.name()));
                        w.list.append(&row);
                    }
                }
                Category::Networks => {
                    w.list.append(&new_row("＋  New network", sender, || Msg::NewNetwork));
                    if snap.networks.is_empty() {
                        w.list.append(&dim_row(if snap.connected { "No networks" } else { "—" }));
                    }
                    for n in &snap.networks {
                        let row = network_list_row(n);
                        row.set_widget_name(&format!("n:{}", n.id));
                        w.list.append(&row);
                    }
                }
                Category::Volumes => {
                    w.list.append(&new_row("＋  New volume", sender, || Msg::NewVolume));
                    if snap.volumes.is_empty() {
                        w.list.append(&dim_row(if snap.connected { "No volumes" } else { "—" }));
                    }
                    for v in &snap.volumes {
                        let row = volume_list_row(v);
                        row.set_widget_name(&format!("v:{}", v.name));
                        w.list.append(&row);
                    }
                }
                Category::Home | Category::Settings | Category::System => {}
            }
            w.list.unselect_all();
            match &m.selection {
                Selection::Container(id) => select_named(&w.list, &format!("c:{id}")),
                Selection::Image(name) => select_named(&w.list, &format!("i:{name}")),
                Selection::Network(id) => select_named(&w.list, &format!("n:{id}")),
                Selection::Volume(name) => select_named(&w.list, &format!("v:{name}")),
                Selection::None => {}
            }
            // Re-apply the ⌘-batch highlight after a rebuild; drop batch entries that no longer exist.
            let mut present = std::collections::HashSet::new();
            let mut i = 0;
            while let Some(row) = w.list.row_at_index(i) {
                let key = row.widget_name().to_string();
                if w.batch.borrow().contains(&key) {
                    row.add_css_class("dd-batch");
                }
                present.insert(key);
                i += 1;
            }
            w.batch.borrow_mut().retain(|k| present.contains(k));
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
            Selection::Network(id) => match snap.networks.iter().find(|n| &n.id == id) {
                Some(n) => network_detail(n, &snap.containers, sender),
                None => placeholder("This network no longer exists."),
            },
            Selection::Volume(name) => match snap.volumes.iter().find(|v| &v.name == name) {
                Some(v) => volume_detail(v, &snap.containers, sender),
                None => placeholder("This volume no longer exists."),
            },
            Selection::None => placeholder(if snap.connected { "Select an item." } else { "Daemon not running." }),
        };
        w.detail_info.append(&info);

        // Only CONTAINERS get the tabbed detail (Info / Logs / terminals). Images/networks/volumes have
        // just an Info view — hide the tab bar entirely so they don't inherit the container's tab state.
        if let Selection::Container(id) = &m.selection {
            w.term_notebook.set_show_tabs(true);
            w.logs_scroll.set_visible(true);
            w.add_term.set_visible(true);
            // Terminal pages (index ≥ 2) belong to a container — show them again.
            let mut i = 2;
            while let Some(p) = w.term_notebook.nth_page(Some(i)) { p.set_visible(true); i += 1; }
            let name = snap.containers.iter().find(|c| &c.id == id).map(|c| c.name()).unwrap_or_default();
            *w.term_current.borrow_mut() = Some((id.clone(), name));
            // Feed the ＋ menu the shells detected for THIS container (empty until the probe returns).
            *w.term_shells.borrow_mut() = match &m.shells {
                Some((sid, list)) if sid == id => list.clone(),
                _ => Vec::new(),
            };
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
            // Non-container: collapse to a plain Info view (no tab bar, no terminal/logs leakage).
            w.term_notebook.set_show_tabs(false);
            w.logs_scroll.set_visible(false);
            w.add_term.set_visible(false);
            let mut i = 2;
            while let Some(p) = w.term_notebook.nth_page(Some(i)) { p.set_visible(false); i += 1; }
            w.term_notebook.set_current_page(Some(0));
            *w.term_current.borrow_mut() = None;
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


/// A category nav row: symbolic icon + label.
pub(crate) fn cat_row(icon: &str, label: &str) -> gtk::ListBoxRow {
    let img = gtk::Image::from_icon_name(icon);
    img.set_pixel_size(16);
    let lbl = gtk::Label::new(Some(label));
    lbl.set_xalign(0.0);
    // No ad-hoc vertical margins — the shared `.navigation-sidebar > row` padding governs both sidebars.
    let b = gtk::Box::new(gtk::Orientation::Horizontal, 9);
    b.append(&img);
    b.append(&lbl);
    let row = gtk::ListBoxRow::new();
    row.set_child(Some(&b));
    row
}


// ---- bundle environment ----------------------------------------------------

/// When running from inside `dd.app`, point GTK at the bundled runtime data. No-op for a dev
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


pub(crate) fn set_if_absent(key: &str, val: &OsStr) {
    if std::env::var_os(key).is_none() {
        std::env::set_var(key, val);
    }
}



// ---- headless verification ------------------------------------------------------------------------
// Render the live window to a PNG offscreen so the UI can be verified without an interactive session
// (`DD_SHOT=/path/out.png dd-app` screenshots once, then quits). Uses the window's own GSK renderer
// against a WidgetPaintable — no extra window, no user input. Pair with `GSK_RENDERER=cairo` for a
// deterministic software render.
pub fn screenshot(win: &gtk::ApplicationWindow, path: &str) -> Result<(), String> {
    let w = win.width().max(1);
    let h = win.height().max(1);
    let paintable = gtk::WidgetPaintable::new(Some(win));
    let snapshot = gtk::Snapshot::new();
    PaintableExt::snapshot(&paintable, &snapshot, w as f64, h as f64);
    let node = snapshot.to_node().ok_or("empty render tree (window not drawn yet)")?;
    let renderer = win.renderer().ok_or("window has no GSK renderer (not realized)")?;
    let texture = renderer.render_texture(&node, None);
    texture.save_to_png(path).map_err(|e| e.to_string())
}

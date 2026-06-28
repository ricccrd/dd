#![allow(unused_imports, dead_code)]
use crate::{AppModel, Category, Msg, Selection};
use crate::ui::components::*;
use crate::ui::views::*;
use crate::ui::theme::*;
use ddclient::{Container, Image, Network, Volume};
use gtk::prelude::*;
use relm4::ComponentSender;
use std::ffi::OsStr;


// ---- home dashboard --------------------------------------------------------

pub(crate) fn render_home(home: &gtk::Box, m: &AppModel, sender: &ComponentSender<AppModel>) {
    clear_box(home);
    let snap = &m.snap;

    let title = gtk::Label::new(Some("Overview"));
    title.set_xalign(0.0);
    title.add_css_class("dd-h1");
    home.append(&title);

    // Stats + sparkline charts of recent history (running/containers/images/disk over the last poll window).
    let running = snap.containers.iter().filter(|c| c.running()).count();
    let disk_gb = snap.df.as_ref().map(|d| d.layers_size as f64 / 1.0e9).unwrap_or(0.0);
    let ser = |f: fn(&crate::Sample) -> f64| m.history.iter().map(f).collect::<Vec<f64>>();
    let stats = gtk::Box::new(gtk::Orientation::Horizontal, 12);
    stats.set_homogeneous(true);
    stats.append(&sparkline_card("Running", &running.to_string(), ser(|s| s.running), true));
    stats.append(&sparkline_card("Containers", &snap.containers.len().to_string(), ser(|s| s.containers), false));
    stats.append(&sparkline_card("Images", &snap.images.len().to_string(), ser(|s| s.images), false));
    stats.append(&sparkline_card("Disk", &format!("{disk_gb:.1} GB"), ser(|s| s.disk_gb), false));
    home.append(&stats);

    // Docker context — view/switch which daemon `docker` talks to. Crash-safe: when the docker CLI
    // isn't installed `snap.docker_context` is None and we show a note instead of a control.
    home.append(&context_section(m, sender));

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

// ---- Docker context selector ----------------------------------------------
// A field to view + switch the active `docker` context (which daemon the CLI talks to). It reuses
// the existing crash-safe machinery: `snap.docker_context`/`docker_contexts` come from the docker
// CLI via `.ok()` (never panic), and a change emits `Msg::SetContext` (the same handler the
// top-right selector uses). When the docker CLI isn't installed, `docker_context` is None and we
// render an informational card with an "Install CLI" action instead of a control — no subprocess,
// no panic, dd keeps running.
fn context_section(m: &AppModel, sender: &ComponentSender<AppModel>) -> gtk::Box {
    let snap = &m.snap;
    let wrap = gtk::Box::new(gtk::Orientation::Vertical, 8);

    let h = gtk::Label::new(Some("Docker context"));
    h.set_xalign(0.0);
    h.add_css_class("dd-h2");
    wrap.append(&h);

    let card = gtk::Box::new(gtk::Orientation::Vertical, 8);
    card.add_css_class("dd-step-card");

    match &snap.docker_context {
        // docker CLI present -> a real selector of the available contexts
        Some(active) => {
            let mut ctxs = snap.docker_contexts.clone();
            if ctxs.is_empty() {
                ctxs.push(active.clone());
            }
            if !ctxs.iter().any(|c| c == active) {
                ctxs.insert(0, active.clone());
            }

            let row = gtk::Box::new(gtk::Orientation::Horizontal, 12);
            let lbl = gtk::Label::new(Some("Active context"));
            lbl.set_xalign(0.0);
            lbl.set_hexpand(true); // pushes the dropdown to the right
            row.append(&lbl);

            let refs: Vec<&str> = ctxs.iter().map(|s| s.as_str()).collect();
            let dropdown = gtk::DropDown::from_strings(&refs);
            dropdown.add_css_class("dd-seg");
            dropdown.set_tooltip_text(Some("Switch which Docker context (daemon) the `docker` CLI uses"));
            // Select the active context BEFORE connecting the signal so this programmatic set
            // doesn't fire the handler (which would loop: set_context -> refetch -> re-render).
            if let Some(i) = ctxs.iter().position(|c| c == active) {
                dropdown.set_selected(i as u32);
            }
            let ctxs_cb = ctxs.clone();
            let active_cb = active.clone();
            let s = sender.clone();
            dropdown.connect_selected_notify(move |dd| {
                if let Some(name) = ctxs_cb.get(dd.selected() as usize) {
                    // only act on a real user change (guards against any spurious notify)
                    if name != &active_cb {
                        s.input(Msg::SetContext(name.clone()));
                    }
                }
            });
            row.append(&dropdown);
            card.append(&row);

            let hint = gtk::Label::new(Some(
                "Pick which daemon `docker` commands talk to. Choose 'dd' to use the no-VM runtime.",
            ));
            hint.set_xalign(0.0);
            hint.set_wrap(true);
            hint.add_css_class("dd-sub");
            card.append(&hint);
        }
        // docker CLI absent -> friendly note + install action, never a crash
        None => {
            let note = gtk::Label::new(Some(
                "Docker CLI not detected. Install the dd CLI to add the 'dd' context and switch here \
                 — dd keeps running either way.",
            ));
            note.set_xalign(0.0);
            note.set_wrap(true);
            note.add_css_class("dd-sub");
            card.append(&note);
            card.append(&action_row(
                "Install the dd CLI",
                "Sets up the `dd`/`docker` context so you can select it here.",
                "Install CLI",
                false,
                sender,
                || Msg::InstallCli,
            ));
        }
    }

    wrap.append(&card);
    wrap
}

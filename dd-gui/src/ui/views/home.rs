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

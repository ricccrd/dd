#![allow(unused_imports, dead_code)]
use crate::{AppModel, Category, Msg, Selection};
use crate::ui::components::*;
use crate::ui::views::*;
use crate::ui::theme::*;
use ddclient::{Container, Image, Network, Volume};
use gtk::prelude::*;
use relm4::ComponentSender;
use std::ffi::OsStr;


/// A container list row: short id on top, "image · state" below, with a running/stopped dot.
pub(crate) fn container_list_row(c: &Container, removing: bool) -> gtk::ListBoxRow {
    let dot = gtk::Box::new(gtk::Orientation::Horizontal, 0);
    dot.add_css_class("dd-dot");
    dot.add_css_class(if removing { "warn" } else if c.running() { "success" } else { "error" });
    dot.set_valign(gtk::Align::Center);

    let id = gtk::Label::new(Some(&c.short_id()));
    id.set_xalign(0.0);
    id.add_css_class("dd-listrow-title"); // same weight as every other list (was bold "heading")
    let state = if removing {
        "removing…".to_string()
    } else if c.running() {
        "running".to_string()
    } else {
        c.display_status()
    };
    let sub = gtk::Label::new(Some(&format!("{} · {}", c.image, state)));
    sub.set_xalign(0.0);
    sub.set_ellipsize(gtk::pango::EllipsizeMode::End);
    sub.add_css_class("dd-listrow-sub");
    let v = gtk::Box::new(gtk::Orientation::Vertical, 1);
    v.set_hexpand(true);
    v.append(&id);
    v.append(&sub);

    // No ad-hoc margins — the shared `.navigation-sidebar > row` padding governs both sidebars.
    let h = gtk::Box::new(gtk::Orientation::Horizontal, 9);
    h.append(&dot);
    h.append(&v);
    let row = gtk::ListBoxRow::new();
    row.set_child(Some(&h));
    row
}


// ---- detail builders -------------------------------------------------------

pub(crate) fn container_info(c: &Container, networks: &[Network], sender: &ComponentSender<AppModel>) -> gtk::Widget {
    let root = detail_root();

    // Header: state-aware lifecycle actions, then Delete.
    let id = c.id.clone();
    let mut actions = Vec::new();
    if c.paused() {
        actions.push(text_btn("Resume", "suggested-action", sender, { let id = id.clone(); move || Msg::UnpauseContainer(id.clone()) }));
        actions.push(text_btn("Stop", "", sender, { let id = id.clone(); move || Msg::StopContainer(id.clone()) }));
    } else if c.running() {
        actions.push(text_btn("Stop", "suggested-action", sender, { let id = id.clone(); move || Msg::StopContainer(id.clone()) }));
        actions.push(text_btn("Restart", "", sender, { let id = id.clone(); move || Msg::RestartContainer(id.clone()) }));
        actions.push(text_btn("Pause", "", sender, { let id = id.clone(); move || Msg::PauseContainer(id.clone()) }));
    } else {
        actions.push(text_btn("Start", "suggested-action", sender, { let id = id.clone(); move || Msg::StartContainer(id.clone()) }));
    }
    actions.push(text_btn("Delete", "dd-danger", sender, { let id = id.clone(); move || Msg::RemoveContainer(id.clone()) }));
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
        .filter(|n| n.containers.iter().any(|cid| cid == &c.id))
        .map(|n| n.name.clone())
        .collect();
    let ports = c.ports_str();

    root.append(&section("Ports", &if ports.is_empty() { vec![] } else { vec![ports] }));
    root.append(&section("Volumes", &mounts));
    root.append(&section("Networks", &nets));
    root.upcast()
}

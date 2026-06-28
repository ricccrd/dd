#![allow(unused_imports, dead_code)]
use crate::{AppModel, Category, Msg, Selection};
use crate::ui::components::*;
use crate::ui::views::*;
use crate::ui::theme::*;
use ddclient::{Container, Image, Network, Volume};
use gtk::prelude::*;
use relm4::ComponentSender;
use std::ffi::OsStr;


/// The onboarding/splash view: a 2-column layout — branding + enable on the left, CLI install on
/// the right.
pub(crate) fn build_onboarding(sender: &ComponentSender<AppModel>) -> (gtk::Widget, gtk::Label) {
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

use crate::ui::logo_path;

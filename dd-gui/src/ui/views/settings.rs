#![allow(unused_imports, dead_code)]
use crate::{AppModel, Category, Msg, Selection};
use crate::ui::components::*;
use crate::ui::views::*;
use crate::ui::theme::*;
use ddclient::{Container, Image, Network, Volume};
use gtk::prelude::*;
use relm4::ComponentSender;
use std::ffi::OsStr;


/// The Settings page: version, locations, CLI install, and the reset (danger) action.
pub(crate) fn render_settings(s: &gtk::Box, m: &AppModel, sender: &ComponentSender<AppModel>) {
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
    rbtn.add_css_class("dd-danger");
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


pub(crate) fn update_card(version: &str, sender: &ComponentSender<AppModel>) -> gtk::Widget {
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

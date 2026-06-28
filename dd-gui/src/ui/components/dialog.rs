#![allow(unused_imports, dead_code)]
use crate::{AppModel, Category, Msg, Selection};
use crate::ui::components::*;
use crate::ui::views::*;
use crate::ui::theme::*;
use ddclient::{Container, Image, Network, Volume};
use gtk::prelude::*;
use relm4::ComponentSender;
use std::ffi::OsStr;


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


pub(crate) fn detect_shell_index() -> u32 {
    let sh = std::env::var("SHELL").unwrap_or_default();
    if sh.contains("fish") {
        2
    } else if sh.contains("bash") {
        1
    } else {
        0 // zsh (macOS default) or unknown
    }
}


pub(crate) fn shell_instr(idx: u32, on_path: bool, cmd: &str) -> String {
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
    ok.add_css_class("dd-danger");
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
    win.add_css_class("dd-modal");
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
    win.add_css_class("dd-modal");

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


/// A small modal name-entry dialog (used to create networks/volumes). On Create it sends `make(name)`.
pub fn prompt_name(
    parent: &gtk::ApplicationWindow,
    title: &str,
    placeholder: &str,
    sender: &ComponentSender<AppModel>,
    make: fn(String) -> Msg,
) {
    let v = gtk::Box::new(gtk::Orientation::Vertical, 12);
    v.set_margin_top(18);
    v.set_margin_bottom(18);
    v.set_margin_start(20);
    v.set_margin_end(20);
    v.add_css_class("dd-dialog");

    let t = gtk::Label::new(Some(title));
    t.set_xalign(0.0);
    t.add_css_class("dd-onboard-head");

    let entry = gtk::Entry::new();
    entry.set_placeholder_text(Some(placeholder));
    entry.set_activates_default(true);
    entry.set_width_request(240);

    let btns = gtk::Box::new(gtk::Orientation::Horizontal, 8);
    btns.set_halign(gtk::Align::End);
    let cancel = gtk::Button::with_label("Cancel");
    cancel.add_css_class("dd-btn");
    let ok = gtk::Button::with_label("Create");
    ok.add_css_class("dd-btn");
    ok.add_css_class("suggested-action");
    btns.append(&cancel);
    btns.append(&ok);

    v.append(&t);
    v.append(&entry);
    v.append(&btns);

    let win = gtk::Window::builder().modal(true).resizable(false).decorated(false).child(&v).build();
    win.set_transient_for(Some(parent));
    win.add_css_class("dd-modal");

    let w1 = win.clone();
    cancel.connect_clicked(move |_| w1.close());
    let s = sender.clone();
    let w2 = win.clone();
    let e = entry.clone();
    ok.connect_clicked(move |_| {
        let name = e.text().as_str().trim().to_string();
        if !name.is_empty() {
            s.input(make(name));
        }
        w2.close();
    });
    win.present();
}

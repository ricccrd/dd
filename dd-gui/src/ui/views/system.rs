#![allow(unused_imports, dead_code)]
use crate::{AppModel, Category, Msg, Selection};
use crate::ui::components::*;
use crate::ui::views::*;
use crate::ui::theme::*;
use ddclient::{Container, Image, Network, Volume};
use gtk::prelude::*;
use relm4::ComponentSender;
use std::ffi::OsStr;

/// The System view: engine info, resource counts, disk usage, and a live tail of the daemon's own log
/// — i.e. complete observability of the running daemon on one page.
pub(crate) fn render_system(s: &gtk::Box, m: &AppModel, _sender: &ComponentSender<AppModel>) {
    clear_box(s);
    let snap = &m.snap;

    if !snap.connected {
        let l = gtk::Label::new(Some("Daemon not running — start it to see engine details and logs."));
        l.set_xalign(0.0);
        l.add_css_class("dd-empty");
        s.append(&l);
        return;
    }

    // Engine info (counts live on Home now).
    if let Some(sys) = &snap.sys {
        let osarch = format!("{} · {}", sys.os, sys.arch);
        let cpus = sys.ncpu.to_string();
        let mem = if sys.mem_total > 0 { human_size(sys.mem_total) } else { "—".to_string() };
        let engine = h2("Engine");
        s.append(&engine);
        s.append(&setting_card(&[
            ("Server version", sys.server_version.as_str()),
            ("API version", sys.api_version.as_str()),
            ("OS / Arch", osarch.as_str()),
            ("Kernel", sys.kernel.as_str()),
            ("Storage driver", sys.driver.as_str()),
            ("Root dir", sys.root_dir.as_str()),
            ("CPUs", cpus.as_str()),
            ("Memory", mem.as_str()),
        ]));
    }

    // Disk usage.
    if let Some(df) = &snap.df {
        let layers = human_size(df.layers_size);
        let imgs = df.images.to_string();
        let ctrs = df.containers.to_string();
        let vols = df.volumes.to_string();
        s.append(&h2("Disk usage"));
        s.append(&setting_card(&[
            ("Image layers", layers.as_str()),
            ("Images", imgs.as_str()),
            ("Containers", ctrs.as_str()),
            ("Volumes", vols.as_str()),
        ]));
    }

    // Daemon log — what the daemon itself is logging (tail).
    s.append(&h2("Daemon log"));
    let tv = gtk::TextView::new();
    tv.set_editable(false);
    tv.set_cursor_visible(false);
    tv.set_monospace(true);
    tv.add_css_class("dd-logs");
    tv.set_left_margin(10);
    tv.set_top_margin(8);
    tv.set_bottom_margin(8);
    let text = if snap.daemon_log.trim().is_empty() {
        "(no daemon log output found at ~/Library/Logs/dd/daemon.err.log)".to_string()
    } else {
        snap.daemon_log.clone()
    };
    tv.buffer().set_text(&text);
    let mut end = tv.buffer().end_iter();
    tv.scroll_to_iter(&mut end, 0.0, false, 0.0, 0.0);
    let sw = gtk::ScrolledWindow::builder().child(&tv).vexpand(true).hexpand(true).min_content_height(260).build();
    sw.add_css_class("dd-content");
    s.append(&sw);
}

/// A section heading used between cards on the dashboard-style pages.
fn h2(text: &str) -> gtk::Label {
    let l = gtk::Label::new(Some(text));
    l.set_xalign(0.0);
    l.add_css_class("dd-h2");
    l
}

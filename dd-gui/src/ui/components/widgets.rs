#![allow(unused_imports, dead_code)]
use crate::{AppModel, Category, Msg, Selection};
use crate::ui::components::*;
use crate::ui::views::*;
use crate::ui::theme::*;
use ddclient::{Container, Image, Network, Volume};
use gtk::prelude::*;
use relm4::ComponentSender;
use std::ffi::OsStr;


/// A card of key/value rows (selectable monospace values) for the Settings page.
pub(crate) fn setting_card(rows: &[(&str, &str)]) -> gtk::Box {
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


pub(crate) fn stat_card(value: &str, name: &str, accent: bool) -> gtk::Widget {
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


/// A "title + description on the left, action button on the right" row for the Get Started card.
pub(crate) fn action_row(
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


/// Format a byte count compactly (B / KB / MB / GB).
pub(crate) fn human_size(bytes: i64) -> String {
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


pub(crate) fn detail_root() -> gtk::Box {
    let b = gtk::Box::new(gtk::Orientation::Vertical, 18);
    b.set_margin_top(22);
    b.set_margin_bottom(22);
    b.set_margin_start(24);
    b.set_margin_end(24);
    b
}


pub(crate) fn detail_header(title: &str, subtitle: &str, actions: Vec<gtk::Button>) -> gtk::Widget {
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
pub(crate) fn section(title: &str, lines: &[String]) -> gtk::Widget {
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

pub(crate) fn nav_list() -> gtk::ListBox {
    let l = gtk::ListBox::new();
    l.set_selection_mode(gtk::SelectionMode::Single);
    l.add_css_class("navigation-sidebar");
    l
}


pub(crate) fn nav_item(title: &str, subtitle: &str, running: bool) -> gtk::ListBoxRow {
    // No ad-hoc margins — the shared `.navigation-sidebar > row` padding governs both sidebars.
    let v = gtk::Box::new(gtk::Orientation::Vertical, 1);
    let t = gtk::Label::new(Some(title));
    t.set_xalign(0.0);
    t.set_ellipsize(gtk::pango::EllipsizeMode::End);
    t.add_css_class("dd-listrow-title"); // same title weight as the containers list
    v.append(&t);
    if !subtitle.is_empty() {
        let s = gtk::Label::new(Some(subtitle));
        s.set_xalign(0.0);
        s.add_css_class("dd-listrow-sub");
        if running { s.add_css_class("success"); }
        v.append(&s);
    }
    let row = gtk::ListBoxRow::new();
    row.set_child(Some(&v));
    row
}


pub(crate) fn section_caption(text: &str) -> gtk::Label {
    let l = gtk::Label::new(Some(&text.to_uppercase()));
    l.set_xalign(0.0);
    l.add_css_class("dd-section-title");
    l.set_margin_top(10);
    l.set_margin_bottom(2);
    l.set_margin_start(12);
    l
}


pub(crate) fn dim_row(text: &str) -> gtk::ListBoxRow {
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

pub(crate) fn text_btn(
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


pub(crate) fn placeholder(text: &str) -> gtk::Widget {
    let l = gtk::Label::new(Some(text));
    l.add_css_class("dim-label");
    l.set_vexpand(true);
    l.set_hexpand(true);
    l.set_valign(gtk::Align::Center);
    l.set_halign(gtk::Align::Center);
    l.upcast()
}


pub(crate) fn select_named(list: &gtk::ListBox, name: &str) {
    let mut i = 0;
    while let Some(row) = list.row_at_index(i) {
        if row.widget_name().as_str() == name {
            list.select_row(Some(&row));
            return;
        }
        i += 1;
    }
}


pub(crate) fn clear(list: &gtk::ListBox) {
    while let Some(child) = list.first_child() {
        list.remove(&child);
    }
}


pub(crate) fn clear_box(b: &gtk::Box) {
    while let Some(child) = b.first_child() {
        b.remove(&child);
    }
}


// ---- networks / volumes (list rows + detail) ------------------------------------------------------

/// A frameless "＋ New …" action row at the top of a resource list (sends its Msg on click).
pub(crate) fn new_row(label: &str, sender: &ComponentSender<AppModel>, make: impl Fn() -> Msg + 'static) -> gtk::ListBoxRow {
    let b = gtk::Button::with_label(label);
    b.set_has_frame(false);
    b.set_halign(gtk::Align::Start);
    b.add_css_class("dd-popitem");
    let s = sender.clone();
    b.connect_clicked(move |_| s.input(make()));
    let row = gtk::ListBoxRow::new();
    row.set_selectable(false);
    row.set_activatable(false);
    row.set_child(Some(&b));
    row
}

// ---- sparkline stat card --------------------------------------------------------------------------

/// A dashboard card: a big current value, a caption, and a sparkline of its recent history.
pub(crate) fn sparkline_card(title: &str, value: &str, series: Vec<f64>, accent: bool) -> gtk::Widget {
    let card = gtk::Box::new(gtk::Orientation::Vertical, 6);
    card.add_css_class("dd-stat-card");
    card.set_hexpand(true);

    let val = gtk::Label::new(Some(value));
    val.set_xalign(0.0);
    val.add_css_class("dd-stat-value");
    if accent { val.add_css_class("accent"); }
    let name = gtk::Label::new(Some(&title.to_uppercase()));
    name.set_xalign(0.0);
    name.add_css_class("dd-stat-name");
    card.append(&val);
    card.append(&name);

    let area = gtk::DrawingArea::new();
    area.set_content_height(38);
    area.set_hexpand(true);
    area.set_margin_top(4);
    area.set_draw_func(move |_, cr, w, h| draw_sparkline(cr, w, h, &series, accent));
    card.append(&area);
    card.upcast()
}

fn draw_sparkline(cr: &gtk::cairo::Context, w: i32, h: i32, data: &[f64], accent: bool) {
    if data.len() < 2 {
        return;
    }
    let (w, h) = (w as f64, h as f64);
    let (mut lo, mut hi) = (f64::MAX, f64::MIN);
    for &v in data { lo = lo.min(v); hi = hi.max(v); }
    if (hi - lo).abs() < 1e-9 { hi = lo + 1.0; }
    let n = data.len();
    let x = |i: usize| (i as f64) / ((n - 1) as f64) * w;
    let y = |v: f64| h - 3.0 - ((v - lo) / (hi - lo)) * (h - 6.0);
    let (r, g, b) = if accent { (0.17, 0.79, 0.35) } else { (0.04, 0.52, 1.0) };

    // soft area fill under the curve
    cr.move_to(0.0, h);
    for (i, &v) in data.iter().enumerate() { cr.line_to(x(i), y(v)); }
    cr.line_to(w, h);
    cr.close_path();
    cr.set_source_rgba(r, g, b, 0.10);
    let _ = cr.fill();

    // the line itself
    for (i, &v) in data.iter().enumerate() {
        if i == 0 { cr.move_to(x(i), y(v)); } else { cr.line_to(x(i), y(v)); }
    }
    cr.set_source_rgba(r, g, b, 0.9);
    cr.set_line_width(1.6);
    let _ = cr.stroke();
}

/// A compact key/value detail block (denser than stacked sections). Empty values show an em-dash.
pub(crate) fn two_col(rows: &[(&str, String)]) -> gtk::Widget {
    let card = gtk::Box::new(gtk::Orientation::Vertical, 0);
    for (k, v) in rows {
        let row = gtk::Box::new(gtk::Orientation::Horizontal, 10);
        row.set_margin_top(3);
        row.set_margin_bottom(3);
        let key = gtk::Label::new(Some(k));
        key.set_xalign(0.0);
        key.add_css_class("dd-kv-key");
        let val = gtk::Label::new(Some(if v.is_empty() { "—" } else { v.as_str() }));
        val.set_xalign(0.0);
        val.set_hexpand(true);
        val.set_wrap(true);
        // Only real values are selectable — an empty "—" shouldn't show a text cursor / be clickable.
        val.set_selectable(!v.is_empty());
        val.add_css_class("dd-kv-val");
        row.append(&key);
        row.append(&val);
        card.append(&row);
    }
    card.upcast()
}

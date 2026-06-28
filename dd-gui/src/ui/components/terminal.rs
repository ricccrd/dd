#![allow(unused_imports, dead_code)]
use crate::{AppModel, Category, Msg, Selection};
use gtk::prelude::*;
use std::cell::RefCell;
use std::rc::Rc;
use vte4::prelude::*;

/// Selected container the "＋" button opens a shell into: (id, display name). Shared between `render`
/// (which keeps it current) and the new-terminal button (which reads it on click).
pub(crate) type TermTarget = Rc<RefCell<Option<(String, String)>>>;
/// Shells detected in the current container (basenames, preference-ordered). `render` keeps it current.
pub(crate) type TermShells = Rc<RefCell<Vec<String>>>;

/// The "＋" control: a menu of the shells actually DETECTED in the selected container (populated fresh
/// each time it opens). Each opens an interactive `docker exec` tab.
pub(crate) fn new_terminal_button(nb: &gtk::Notebook, target: TermTarget, shells: TermShells) -> gtk::Widget {
    let menu = gtk::MenuButton::new();
    menu.set_icon_name("list-add-symbolic");
    menu.set_tooltip_text(Some("Open a shell in the selected container"));
    menu.add_css_class("dd-seg");
    menu.set_margin_end(4);
    menu.set_valign(gtk::Align::Center); // keep the ＋ from dictating the tab-strip height

    // Build the popover lazily on each open, from the freshly-detected shell list.
    let nb = nb.clone();
    menu.set_create_popup_func(move |mb| {
        let pop_box = gtk::Box::new(gtk::Orientation::Vertical, 1);
        pop_box.set_size_request(160, -1);
        let detected = shells.borrow().clone();
        let opts: Vec<String> = if detected.is_empty() { vec!["sh".to_string()] } else { detected };
        for shell in opts {
            let lbl = gtk::Label::new(Some(&shell));
            lbl.set_xalign(0.0);
            lbl.set_hexpand(true);
            let item = gtk::Button::builder().child(&lbl).build();
            item.set_has_frame(false);
            item.add_css_class("dd-popitem");
            let nb = nb.clone();
            let target = target.clone();
            let mb = mb.clone();
            let shell = shell.clone();
            item.connect_clicked(move |_| {
                if let Some((id, name)) = target.borrow().clone() {
                    open_terminal_tab(&nb, &id, &name, &shell);
                }
                mb.popdown();
            });
            pop_box.append(&item);
        }
        let pop = gtk::Popover::new();
        pop.set_has_arrow(false);
        pop.add_css_class("dd-pop");
        pop.set_child(Some(&pop_box));
        mb.set_popover(Some(&pop));
    });
    menu.upcast()
}

/// Open an interactive terminal tab: a styled VTE running `docker exec -it <id> <shell>` (VTE provides
/// the PTY, docker bridges to the container — works the same on macOS and Linux).
pub(crate) fn open_terminal_tab(nb: &gtk::Notebook, id: &str, _name: &str, shell: &str) {
    let term = vte4::Terminal::new();
    style_terminal(&term);

    let host = daemon_host();
    let docker = docker_bin();
    // `shell` may be e.g. "busybox sh" → split into argv words. `-e TERM=…` so the shell gets a real
    // terminfo (arrow keys, line editing, colors); without it `bash`/`zsh` arrows misbehave. (`sh`/dash
    // has no line-editing/history regardless — pick bash or zsh from the ＋ menu for history.)
    let shell_words: Vec<&str> = shell.split_whitespace().collect();
    let mut argv: Vec<&str> = vec![&docker, "--host", &host, "exec", "-it", "-e", "TERM=xterm-256color", id];
    argv.extend(&shell_words);

    // Inherit the environment but guarantee a PATH that can find `docker` + the container tools.
    let mut env: Vec<String> = std::env::vars().map(|(k, v)| format!("{k}={v}")).collect();
    if !env.iter().any(|e| e.starts_with("PATH=")) {
        env.push("PATH=/usr/local/bin:/opt/homebrew/bin:/usr/bin:/bin".into());
    }
    let envv: Vec<&str> = env.iter().map(|s| s.as_str()).collect();

    // The spawned `docker exec` pid, captured so closing the tab can kill the whole process tree.
    let pid_cell: Rc<std::cell::Cell<i32>> = Rc::new(std::cell::Cell::new(0));
    let term_cb = term.clone();
    let shell_owned = shell.to_string();
    let pid_cb = pid_cell.clone();
    term.spawn_async(
        vte4::PtyFlags::DEFAULT,
        None,
        &argv,
        &envv,
        gtk::glib::SpawnFlags::DEFAULT,
        || {},
        -1,
        None::<&gtk::gio::Cancellable>,
        move |res| match res {
            Ok(pid) => pid_cb.set(pid.0 as i32),
            Err(e) => term_cb.feed(format!("\r\n\x1b[31mfailed to start `{shell_owned}`: {e}\x1b[0m\r\n").as_bytes()),
        },
    );

    // When the shell exits, show it (don't silently blank the tab).
    let nb_done = nb.clone();
    term.connect_child_exited(move |t, status| {
        t.feed(format!("\r\n\x1b[2m[process exited {status}]\x1b[0m\r\n").as_bytes());
        let _ = &nb_done;
    });

    // Cmd+C / Cmd+V copy & paste (VTE doesn't bind these itself). Cmd is META on macOS.
    let keys = gtk::EventControllerKey::new();
    let t = term.clone();
    keys.connect_key_pressed(move |_, keyval, _kc, mods| {
        if mods.contains(gtk::gdk::ModifierType::META_MASK) {
            match keyval {
                gtk::gdk::Key::c | gtk::gdk::Key::C => {
                    t.copy_clipboard_format(vte4::Format::Text);
                    return gtk::glib::Propagation::Stop;
                }
                gtk::gdk::Key::v | gtk::gdk::Key::V => {
                    t.paste_clipboard();
                    return gtk::glib::Propagation::Stop;
                }
                _ => {}
            }
        }
        gtk::glib::Propagation::Proceed
    });
    term.add_controller(keys);

    let scroll = gtk::ScrolledWindow::builder().child(&term).vexpand(true).hexpand(true).build();
    let tab = closable_tab(nb, &scroll, &format!("{}: {}", short(id), shell), pid_cell);
    let page = nb.append_page(&scroll, Some(&tab));
    nb.set_tab_reorderable(&scroll, true);
    nb.set_current_page(Some(page));
    term.grab_focus();
}

/// A tab label with a title + a small close button that removes the page.
fn closable_tab(nb: &gtk::Notebook, child: &impl IsA<gtk::Widget>, title: &str, pid: Rc<std::cell::Cell<i32>>) -> gtk::Widget {
    let row = gtk::Box::new(gtk::Orientation::Horizontal, 7);
    row.set_valign(gtk::Align::Center);
    let lbl = gtk::Label::new(Some(title));
    lbl.set_valign(gtk::Align::Center);
    // The close affordance is a LABEL (✕), not a Button — a button's intrinsic min-height/padding makes
    // the terminal tab taller than the plain Info/Logs label tabs, which grows the whole tab strip.
    let close = gtk::Label::new(Some("✕"));
    close.set_valign(gtk::Align::Center);
    close.add_css_class("dd-tabclose");
    let click = gtk::GestureClick::new();
    let nb = nb.clone();
    let child = child.clone().upcast::<gtk::Widget>();
    click.connect_released(move |_, _, _, _| {
        // Kill the whole `docker exec` process tree (VTE put it in its own session, so pgid == pid),
        // then drop the tab — closing the tab must actually terminate the shell.
        let p = pid.get();
        if p > 0 {
            let _ = std::process::Command::new("kill").arg("-KILL").arg(format!("-{p}")).status();
            let _ = std::process::Command::new("kill").arg("-KILL").arg(p.to_string()).status();
        }
        if let Some(pg) = nb.page_num(&child) {
            nb.remove_page(Some(pg));
        }
    });
    close.add_controller(click);
    row.append(&lbl);
    row.append(&close);
    row.upcast()
}

/// Style a VTE to match the app: SF Mono, a soft-dark palette, generous scrollback, no bell.
fn style_terminal(term: &vte4::Terminal) {
    // Menlo is the clean macOS terminal default and always present; fall back to a generic monospace
    // elsewhere. A touch of line spacing + slightly larger size reads much better than SF Mono here.
    let mut font = gtk::pango::FontDescription::from_string("Menlo 13");
    if font.family().is_none() {
        font = gtk::pango::FontDescription::from_string("monospace 13");
    }
    term.set_font(Some(&font));
    term.set_cell_height_scale(1.06);
    term.set_scrollback_lines(10_000);
    term.set_audible_bell(false);
    term.set_hexpand(true);
    term.set_vexpand(true);
    let fg = gtk::gdk::RGBA::parse("#e8e8ea").unwrap();
    let bg = gtk::gdk::RGBA::parse("#1b1d22").unwrap();
    term.set_colors(Some(&fg), Some(&bg), &[]);
}

/// `unix://<socket>` for `docker --host` ( `$DDOCKERD_SOCK`, else `~/.dd/run/docker.sock` ).
fn daemon_host() -> String {
    let sock = std::env::var("DDOCKERD_SOCK").unwrap_or_else(|_| {
        let home = std::env::var("HOME").unwrap_or_else(|_| ".".into());
        format!("{home}/.dd/run/docker.sock")
    });
    format!("unix://{sock}")
}

/// Resolve the `docker` CLI (the bundle's PATH is minimal, so probe the usual spots).
pub(crate) fn docker_bin() -> String {
    for p in ["/usr/local/bin/docker", "/opt/homebrew/bin/docker", "/usr/bin/docker"] {
        if std::path::Path::new(p).exists() {
            return p.to_string();
        }
    }
    "docker".to_string()
}

fn short(id: &str) -> String {
    id.trim_start_matches("sha256:").chars().take(12).collect()
}

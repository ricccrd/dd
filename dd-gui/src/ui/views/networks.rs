#![allow(unused_imports, dead_code)]
use crate::{AppModel, Category, Msg, Selection};
use crate::ui::components::*;
use crate::ui::views::*;
use crate::ui::theme::*;
use ddclient::{Container, Image, Network, Volume};
use gtk::prelude::*;
use relm4::ComponentSender;
use std::ffi::OsStr;


pub(crate) fn network_list_row(n: &Network) -> gtk::ListBoxRow {
    // Richer subtitle: driver · scope (· subnet when present).
    let sub = if n.subnet.is_empty() {
        format!("{} · {}", n.driver, n.scope)
    } else {
        format!("{} · {}", n.driver, n.subnet)
    };
    nav_item(&n.name, &sub, false)
}


pub(crate) fn network_detail(n: &Network, containers: &[Container], sender: &ComponentSender<AppModel>) -> gtk::Widget {
    let root = detail_root();
    let mut actions = Vec::new();
    // The predefined bridge/host/none networks can't be removed.
    if !matches!(n.name.as_str(), "bridge" | "host" | "none") {
        actions.push(text_btn("Delete", "dd-danger", sender, {
            let id = n.id.clone();
            move || Msg::RemoveNetwork(id.clone())
        }));
    }
    root.append(&detail_header(&n.name, &format!("{} · {}", n.driver, n.scope), actions));
    root.append(&section("ID", &[n.short_id()]));
    root.append(&section("Driver", &[n.driver.clone()]));

    let mut flags = Vec::new();
    if n.internal { flags.push("internal"); }
    if n.attachable { flags.push("attachable"); }
    if n.ipv6 { flags.push("ipv6"); }
    root.append(&two_col(&[
        ("Scope", n.scope.clone()),
        ("Subnet", n.subnet.clone()),
        ("Gateway", n.gateway.clone()),
        ("Flags", flags.join(", ")),
    ]));

    let labels: Vec<String> = n.labels.iter().map(|(k, v)| format!("{k} = {v}")).collect();
    root.append(&section("Labels", &labels));
    let opts: Vec<String> = n.options.iter().map(|(k, v)| format!("{k} = {v}")).collect();
    root.append(&section("Options", &opts));

    let conn: Vec<String> = containers
        .iter()
        .filter(|c| n.containers.iter().any(|cid| cid == &c.id))
        .map(|c| c.name())
        .collect();
    root.append(&section("Containers", &conn));
    root.upcast()
}

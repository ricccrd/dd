#![allow(unused_imports, dead_code)]
use crate::{AppModel, Category, Msg, Selection};
use crate::ui::components::*;
use crate::ui::views::*;
use crate::ui::theme::*;
use ddclient::{Container, Image, Network, Volume};
use gtk::prelude::*;
use relm4::ComponentSender;
use std::ffi::OsStr;


pub(crate) fn volume_list_row(v: &Volume) -> gtk::ListBoxRow {
    nav_item(&v.name, &v.driver, false)
}


pub(crate) fn volume_detail(v: &Volume, containers: &[Container], sender: &ComponentSender<AppModel>) -> gtk::Widget {
    let root = detail_root();
    let del = text_btn("Delete", "dd-danger", sender, {
        let name = v.name.clone();
        move || Msg::RemoveVolume(name.clone())
    });
    root.append(&detail_header(&v.name, &v.driver, vec![del]));
    root.append(&two_col(&[
        ("Driver", v.driver.clone()),
        ("Scope", v.scope.clone()),
        ("Mountpoint", v.mountpoint.clone()),
    ]));
    let labels: Vec<String> = v.labels.iter().map(|(k, val)| format!("{k} = {val}")).collect();
    root.append(&section("Labels", &labels));
    let opts: Vec<String> = v.options.iter().map(|(k, val)| format!("{k} = {val}")).collect();
    root.append(&section("Options", &opts));
    let users: Vec<String> = containers
        .iter()
        .filter(|c| c.mounts.iter().any(|m| m.source == v.mountpoint || m.source == v.name))
        .map(|c| c.name())
        .collect();
    root.append(&section("Used by", &users));
    root.upcast()
}

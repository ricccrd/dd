#![allow(unused_imports, dead_code)]
use crate::{AppModel, Category, Msg, Selection};
use crate::ui::components::*;
use crate::ui::views::*;
use crate::ui::theme::*;
use ddclient::{Container, Image, Network, Volume};
use gtk::prelude::*;
use relm4::ComponentSender;
use std::ffi::OsStr;


pub(crate) fn image_detail(img: &Image, sender: &ComponentSender<AppModel>) -> gtk::Widget {
    let root = detail_root();
    let run = text_btn("Run", "suggested-action", sender, {
        let name = img.name();
        move || Msg::RunImage(name.clone())
    });
    let del = text_btn("Delete", "dd-danger", sender, {
        let name = img.name();
        move || Msg::RemoveImage(name.clone())
    });
    let arch = if img.architecture.is_empty() { "image".into() } else { img.architecture.clone() };
    root.append(&detail_header(&img.name(), &arch, vec![run, del]));
    root.append(&two_col(&[
        ("Image ID", img.id.trim_start_matches("sha256:").chars().take(12).collect::<String>()),
        ("Repo tags", if img.repo_tags.is_empty() { String::new() } else { img.repo_tags.join(", ") }),
        ("Size", human_size(img.size)),
        // dd unpacks each image to one rootfs, so there are no docker-style intermediate layers.
        ("Layers", "1 (dd squashes images to a single rootfs)".into()),
    ]));
    root.upcast()
}

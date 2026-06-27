//! macOS-only AppKit shim. Kept in its own module so it doesn't pull in `gtk::prelude::*` (whose
//! extension traits otherwise clash with objc2's NSArray/NSObject method resolution).

use objc2_app_kit::{NSApplication, NSWindowStyleMask, NSWindowTitleVisibility};
use objc2_foundation::MainThreadMarker;

/// `NSWindowTitleHidden` (the enum's raw value; the named constant isn't exported in this version).
const TITLE_HIDDEN: isize = 1;

/// Give the `dd` window a unified title bar: a full-size content view with a transparent, title-less
/// titlebar, so the native traffic-light controls float over our top strip rather than sitting in a
/// separate bar. Safe to call once the NSWindow exists (on GTK realize).
pub fn unify_titlebar() {
    // Called from GTK's realize signal, which always runs on the main thread.
    let mtm = unsafe { MainThreadMarker::new_unchecked() };
    let app = NSApplication::sharedApplication(mtm);
    let windows = app.windows();
    let count = windows.count();
    for i in 0..count {
        unsafe {
            let w = windows.objectAtIndex(i);
            if w.title().to_string() != "dd" {
                continue;
            }
            w.setTitlebarAppearsTransparent(true);
            w.setTitleVisibility(NSWindowTitleVisibility(TITLE_HIDDEN));
            let mask = NSWindowStyleMask(w.styleMask().0 | NSWindowStyleMask::FullSizeContentView.0);
            w.setStyleMask(mask);
        }
    }
}

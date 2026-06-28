#![allow(dead_code)]
use gtk::prelude::*;


// ---- styling ---------------------------------------------------------------

/// Flat, simple, macOS-leaning CSS: gray window, a single floating base-color pane inset with a
/// border radius ("pane in pane"), faintly tinted sidebar, no gradients. Uses theme color names so
/// light/dark both work.
pub(crate) const CSS: &str = "
/* === dd design tokens =============================================================================
   A sharp, precise developer-tool aesthetic from ONE kit: a single 6px radius on every card/control/
   popover/dialog, one hairline border token (@dd_line), an 8px spacing grid, a tight type scale, and a
   single blue accent. Theme colors keep
   light/dark working; the few literal colors are the macOS system palette. */
@define-color dd_accent #0a84ff;
@define-color dd_accent_hi #3a9bff;
@define-color dd_green #2bd158;
@define-color dd_red #ff453a;
@define-color dd_amber #ff9f0a;
@define-color dd_line alpha(@borders, 0.85);
@define-color dd_line_soft alpha(@borders, 0.5);
@define-color dd_fill alpha(@theme_fg_color, 0.06);
@define-color dd_fill_hi alpha(@theme_fg_color, 0.11);
/* Text ramp — controlled by COLOR, not opacity, so secondary text stays crisp and readable. */
@define-color dd_text @theme_fg_color;
@define-color dd_text_dim mix(@theme_fg_color, @theme_base_color, 0.32);
@define-color dd_text_faint mix(@theme_fg_color, @theme_base_color, 0.50);

window {
  background-color: mix(@theme_bg_color, @theme_base_color, 0.5);
  font-family: 'SF Pro Text', 'SF Pro Display', 'Inter', 'Helvetica Neue', -apple-system, sans-serif;
  font-size: 13.5px;
  color: @dd_text;
}
/* Secondary text everywhere reads from one crisp dim color (no more washed-out 0.5 opacity). */
.dim-label, .caption { opacity: 1; color: @dd_text_dim; }
/* Bigger headings use the Display cut for a more premium feel. */
.dd-h1, .dd-bigtitle, .title-1, .title-2, .dd-detail-title, .dd-stat-value {
  font-family: 'SF Pro Display', 'SF Pro Text', 'Inter', sans-serif;
}
.dd-topstrip { background: transparent; }

/* Surfaces: crisp 6px cards, hairline borders, no shadow. */
.dd-content {
  background-color: @theme_base_color;
  border: 1px solid @dd_line;
  border-radius: 6px;
}
.dd-sidebar {
  background-color: alpha(@theme_base_color, 0.45);
  border: 1px solid @dd_line;
  border-radius: 6px;
}

/* The paned handle is just the 8px gap between the two cards. */
paned > separator {
  background-color: transparent; background-image: none; border: none; box-shadow: none; min-width: 8px;
}

/* BOTH sidebars (nav + master list) share these rules → identical padding/rhythm/selection. */
list.navigation-sidebar { background: transparent; padding: 6px 6px; }
list.navigation-sidebar > row {
  border-radius: 6px; margin: 1px 4px; padding: 6px 8px; font-weight: 500;
}
/* Selected row reads as a SELECTION (accent tint + accent text/icon), not a neutral gray button. */
list.navigation-sidebar > row:selected { background-color: alpha(@dd_accent, 0.13); color: @dd_accent; }
list.navigation-sidebar > row:selected label { color: @dd_accent; }
list.navigation-sidebar > row:selected image { color: @dd_accent; opacity: 1; }
list.navigation-sidebar > row:hover:not(:selected) { background-color: @dd_fill; }
list.navigation-sidebar image { opacity: 0.6; }
/* No inset/active shadow or focus ring on rows — kills the inner-shadow flash on click. */
list.navigation-sidebar > row:active { background-color: alpha(@dd_accent, 0.13); }
list.navigation-sidebar > row, list.navigation-sidebar > row:active, list.navigation-sidebar > row:focus {
  box-shadow: none; outline: none;
}
row:focus, row:focus-visible, button:focus, button:focus-visible { outline: none; }
row:active, button:active { box-shadow: none; }
/* ⌘-batch members: an accent left-bar + tint, distinct from the lighter single (view) selection. */
list.navigation-sidebar > row.dd-batch { background-color: alpha(@dd_accent, 0.20); box-shadow: inset 3px 0 0 0 @dd_accent; }

/* Header status (daemon | docker): flat clickable text, no chrome. */
.dd-statusgroup { background: none; border: none; padding: 0; }
.dd-seg {
  background: none; background-color: transparent; border: none; box-shadow: none; outline: none;
  border-radius: 6px; padding: 2px 9px; min-height: 0; font-weight: 500;
}
button.dd-seg:hover { background-color: @dd_fill; }
.dd-seg.dd-active { color: @dd_green; font-weight: 700; }
menubutton.dd-seg, menubutton.dd-seg > button {
  background: none; background-color: transparent; border: none; box-shadow: none; outline: none; min-height: 0;
}
menubutton.dd-seg > button { border-radius: 6px; padding: 3px 8px; font-weight: 500; }
menubutton.dd-seg > button:hover { background-color: @dd_fill; }

/* Status dot. */
.dd-dot { min-width: 8px; min-height: 8px; border-radius: 50%; background-color: alpha(@theme_fg_color, 0.35); }
.dd-dot.success { background-color: @dd_green; }
.dd-dot.error { background-color: @dd_red; }
.dd-dot.warn { background-color: @dd_amber; }

/* Buttons: flat neutral fill. Kill Adwaita gradient background-image (the source of the gray hover
   and the raised, button-like look) on every button state up front. */
button { box-shadow: none; background-image: none; }
button:hover, button:active, button:checked, button:focus { background-image: none; }
button.flat { padding: 4px 7px; border-radius: 6px; background-color: transparent; }
.dd-btn {
  padding: 5px 14px; min-height: 0; font-weight: 600; border: none; box-shadow: none;
  border-radius: 6px; background-color: @dd_fill; background-image: none;
}
.dd-btn:hover { background-color: @dd_fill_hi; background-image: none; }
.dd-btn.suggested-action { background-color: @dd_accent; color: #ffffff; }
.dd-btn.suggested-action:hover { background-color: @dd_accent_hi; }
/* Destructive: our own class (NOT Adwaita's `.destructive-action`, which the theme would override).
   A tinted red outline by default, going solid on hover — deletes read clearly but don't shout. */
.dd-btn.dd-danger { background-color: alpha(@dd_red, 0.12); color: @dd_red; box-shadow: inset 0 0 0 1px alpha(@dd_red, 0.4); }
.dd-btn.dd-danger:hover { background-color: @dd_red; color: #ffffff; box-shadow: none; }

/* Context popover. */
popover.dd-pop { background: transparent; }
popover.dd-pop > arrow { background: transparent; border: none; }
popover.dd-pop > contents { padding: 4px; border-radius: 6px; background-color: @theme_base_color; border: 1px solid @dd_line; }
.dd-popitem {
  background: none; background-color: transparent; border: none; box-shadow: none; outline: none;
  min-height: 0; border-radius: 6px; padding: 5px 10px; font-weight: 400;
}
.dd-popitem:hover { background-color: @dd_fill; }
.dd-popitem.dd-active { color: @dd_accent; font-weight: 600; }

/* Modal dialogs: a solid, clean panel (transparency caused overlap/artefacts on macOS). The window
   carries the app's surface color; the inner box just adds padding. */
window.dd-modal { background-color: @theme_base_color; }
window.dd-modal decoration { background: transparent; box-shadow: none; }
.dd-dialog { background-color: @theme_base_color; }

.dd-update { background-color: @dd_accent; color: #ffffff; border: none; box-shadow: none; border-radius: 6px; padding: 3px 11px; font-weight: 600; min-height: 0; }
.dd-update:hover { background-color: @dd_accent_hi; }

/* Headings + dashboard. */
.dd-home { background: transparent; }
.dd-h1 { font-size: 21px; font-weight: 800; letter-spacing: -0.01em; margin-bottom: 2px; }
.dd-h2 { font-size: 15px; font-weight: 700; margin-top: 6px; }
.dd-stat-card { background-color: @theme_base_color; border: 1px solid @dd_line; border-radius: 6px; padding: 13px 15px; }
.dd-stat-value { font-size: 27px; font-weight: 800; letter-spacing: -0.02em; }
.dd-stat-value.accent { color: @dd_green; }
.dd-stat-name { font-size: 11.5px; font-weight: 600; color: @dd_text_dim; letter-spacing: 0.04em; }
.dd-update-card { background-color: alpha(@dd_accent, 0.10); border: 1px solid alpha(@dd_accent, 0.30); border-radius: 6px; padding: 13px 15px; }
.dd-mono { font-family: 'SF Mono', 'Menlo', monospace; font-size: 12px; }
.dd-step-card { background-color: @theme_base_color; border: 1px solid @dd_line; border-radius: 6px; padding: 11px 15px; }
.dd-code {
  font-family: 'SF Mono', 'Menlo', monospace; font-size: 12px; color: alpha(@theme_fg_color, 0.85);
  background-color: @dd_fill; border: 1px solid @dd_line; border-radius: 6px; padding: 10px 12px;
}

/* Onboarding. */
.dd-bigtitle { font-size: 33px; font-weight: 800; letter-spacing: -0.02em; }
.dd-sub { font-size: 13.5px; color: @dd_text_dim; }
.dd-onboard-status { font-size: 13.5px; font-weight: 600; }
.dd-onboard-head { font-size: 15px; font-weight: 700; }
.dd-cli-msg { font-family: 'SF Mono', 'Menlo', monospace; font-size: 11px; background-color: @dd_fill; border-radius: 6px; padding: 8px 10px; margin-top: 6px; }

/* Logs pane. */
.dd-logs, .dd-logs text { background-color: alpha(@theme_fg_color, 0.035); font-family: 'SF Mono', 'Menlo', monospace; font-size: 12px; }

/* Section label (uppercase, tracked). */
.dd-section-title { font-size: 0.72em; font-weight: 700; color: @dd_text_faint; letter-spacing: 0.07em; }

/* === reusable widgets (used across the resource panels) ============================================ */
/* Detail/list row: a selectable line in a master list. */
.dd-listrow { padding: 7px 10px; border-radius: 6px; }
.dd-listrow:hover { background-color: @dd_fill; }
.dd-listrow-title { font-weight: 600; font-size: 13px; }
.dd-listrow-sub { font-size: 12px; color: @dd_text_dim; }

/* Key/value detail line. */
.dd-kv-key { font-size: 12.5px; color: @dd_text_dim; min-width: 96px; }
.dd-kv-val { font-size: 13px; }

/* Status badge / chip. */
.dd-badge { font-size: 10.5px; font-weight: 700; letter-spacing: 0.03em; padding: 1px 7px; border-radius: 6px; background-color: @dd_fill; }
.dd-badge.run { color: @dd_green; background-color: alpha(@dd_green, 0.14); }
.dd-badge.stop { color: alpha(@theme_fg_color, 0.6); }
.dd-badge.fail { color: @dd_red; background-color: alpha(@dd_red, 0.13); }

/* Detail pane title + toolbar. */
.dd-detail-title { font-size: 17px; font-weight: 800; letter-spacing: -0.01em; }
.dd-detail-sub { font-size: 12.5px; color: @dd_text_dim; }
.dd-toolbar { padding: 0; }

/* Hairline separators between sections. */
.dd-hsep { background-color: @dd_line_soft; min-height: 1px; }
.dd-empty { font-size: 13px; color: @dd_text_faint; }

/* Terminal/Logs dock + System pages: flat underline tabs. EVERY state keeps identical geometry —
   same padding, same constant font-weight, an always-2px bottom border whose COLOR is the only thing
   that changes — so selecting a tab never shifts the layout. No fill, no box: not a button. */
notebook.dd-termbook { background: transparent; border: none; box-shadow: none; padding: 0; }
notebook.dd-termbook > stack { background: transparent; border: none; }
.dd-termbook > header.top {
  background: transparent; border-bottom: 1px solid @dd_line_soft; padding: 0 6px; margin: 0;
}
.dd-termbook > header.top > tabs { margin-bottom: -1px; }
.dd-termbook tab {
  padding: 7px 13px; margin: 0; min-height: 0;
  background: none; background-color: transparent; background-image: none;
  border: none; border-bottom: 2px solid transparent; box-shadow: none; outline: none; border-radius: 0;
  font-size: 12px; font-weight: 500; color: alpha(@theme_fg_color, 0.5);
}
.dd-termbook tab:hover { background: none; color: alpha(@theme_fg_color, 0.85); }
.dd-termbook tab:checked { background: none; color: @theme_fg_color; border-bottom-color: @dd_accent; }
.dd-termbook tab label { color: inherit; font-weight: inherit; }
.dd-tabclose { font-size: 11px; opacity: 0.45; padding: 0 1px; }
.dd-tabclose:hover { opacity: 1; background-color: @dd_fill; }
";


pub(crate) fn load_css() {
    let provider = gtk::CssProvider::new();
    provider.load_from_data(CSS);
    if let Some(display) = gtk::gdk::Display::default() {
        gtk::style_context_add_provider_for_display(
            &display,
            &provider,
            gtk::STYLE_PROVIDER_PRIORITY_APPLICATION,
        );
    }
}

{
  # Dev shell that provides the GTK4 build/runtime deps for `dd-gui` (the `dd-app` GUI) on
  # macOS. Pinned to the exact nixpkgs rev that already has gtk4 cached in this machine's store,
  # so entering the shell never rebuilds GTK. libadwaita is deliberately absent: its `appstream`
  # dependency fails to build under nixpkgs on aarch64-darwin, so dd-app uses pure GTK4.
  #
  #   nix develop ./nix --command bash -lc 'cargo build -p dd-gui'
  description = "dd-app GTK4 dev shell";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/89570f24e97e614aa34aa9ab1c927b6578a43775";

  outputs = { self, nixpkgs }:
    let
      system = "aarch64-darwin";
      pkgs = import nixpkgs { inherit system; };

      # --- macOS dev-container userlands (for `ddcli mac`, built by tools/mac-image.sh) ---------
      # A lean base, and a batteries-included dev set. buildEnv joins them into one profile whose
      # closure tools/mac-image.sh packs into the image rootfs.
      base = with pkgs; [
        bashInteractive coreutils gnugrep gnused gawk findutils diffutils
        less gnutar gzip which ncurses cacert
      ];
      dev = base ++ (with pkgs; [
        # shells + everyday CLI
        zsh fish git curl wget openssh htop tree jq ripgrep fd fzf tmux neovim gnupg
        # build toolchain ("build-essential"-equivalent for macOS)
        gnumake cmake pkg-config clang
        # language toolchains
        python3 nodejs go rustc cargo
      ]);
      mkEnv = name: paths: pkgs.buildEnv { inherit name paths; ignoreCollisions = true; };
    in {
      packages.${system} = {
        mac-base = mkEnv "ddmac-base" base;
        mac-dev  = mkEnv "ddmac-dev"  dev;
        default  = mkEnv "ddmac-dev"  dev;
      };

      devShells.${system}.default = pkgs.mkShell {
        # Build the GUI (pkg-config + gtk4) and assemble/sign the .app + .dmg.
        nativeBuildInputs = [
          pkgs.pkg-config
          pkgs.gobject-introspection
          pkgs.glib                # glib-compile-schemas
          pkgs.gdk-pixbuf          # gdk-pixbuf-query-loaders
          pkgs.macdylibbundler     # provides `dylibbundler` — relocate the dylib graph
          pkgs.create-dmg          # build the .dmg
        ];
        buildInputs = [ pkgs.gtk4 pkgs.librsvg pkgs.vte-gtk4 ];

        # Exported so tools/bundle.sh can find the runtime data to stage (no extra nix calls).
        DD_GTK4 = pkgs.gtk4;
        DD_LIBRSVG = pkgs.librsvg;
        DD_GDK_PIXBUF = pkgs.gdk-pixbuf;
        DD_ADWAITA_ICONS = pkgs.adwaita-icon-theme;
        DD_HICOLOR_ICONS = pkgs.hicolor-icon-theme;
        DD_GSETTINGS_SCHEMAS = pkgs.gsettings-desktop-schemas;
      };
    };
}

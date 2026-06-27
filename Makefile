# dd workspace.
.PHONY: all jit fmt test test-ci test-docker clean app dmg install uninstall
VERSION ?= 0.1.0
# Run a command inside the GTK4 dev shell (provides pkg-config/gtk4 + packaging tools on macOS).
NIX_DEV = nix develop "path:$(CURDIR)/nix" --command
all: jit
jit:            ## build + codesign both guest-arch JITs (via cargo build.rs) + the crates
	cargo build --release
test: jit       ## run the engine × case matrix (grouped report); FILTER=name ENGINE=x86_64 to narrow
	cargo run -q -p dd-tests -- $(if $(ENGINE),-e $(ENGINE)) $(FILTER)
test-ci: jit    ## the cargo-test path (one matrix test; for CI)
	cargo test -p dd-tests
test-docker: jit ## end-to-end Docker-CLI scenarios against dd-daemon (run/logs/stop/kill/volumes/networks)
	bash dd-tests/scenarios/docker.sh
fmt:            ## clang-format the decomposed C (jit/ os/linux/ frontend/ include/ targets/)
	cd dd-jit/src/runtime && find jit os/linux frontend include targets -name '*.c' -o -name '*.h' | xargs clang-format -i
app:            ## build + assemble & ad-hoc-sign build/dd-app.app (the GTK GUI bundle; macOS)
	@chmod +x tools/bundle.sh tools/make-dmg.sh
	cargo build --release -p dd-daemon -p dd-cli   # native toolchain: builds + allow-jit-signs the ddjit-* engines
	$(NIX_DEV) tools/bundle.sh $(VERSION)
dmg: app        ## build dist/dd-<ver>-<arch>.dmg from the app bundle (macOS)
	$(NIX_DEV) tools/make-dmg.sh $(VERSION)
install: app    ## copy the app to /Applications and run `dd install` (per-user, no root)
	rm -rf /Applications/dd-app.app && cp -R target/dd-app.app /Applications/
	cargo run -q -p dd-cli -- install
uninstall:      ## remove the daemon agent + docker context (keeps ~/.dd unless --purge)
	cargo run -q -p dd-cli -- uninstall
clean:
	cargo clean

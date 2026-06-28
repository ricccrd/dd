# dd workspace.
.PHONY: all jit fmt test test-ci test-docker test-docker-full test-compose test-docker-net test-macos test-realsw test-smoke coverage bench clean app dmg install uninstall mac-image mac-push
# Version is the git tag (v0.2.0 -> 0.2.0); falls back to 0.0.0-dev with no tags. CI passes it too.
TAG := $(shell git describe --tags --abbrev=0 2>/dev/null | sed 's/^v//')
VERSION ?= $(or $(TAG),0.0.0-dev)
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
test-docker-full: jit ## FULL Docker CLI/API compliance matrix (every command; maps each failure to a non-compliant verb)
	bash dd-tests/scenarios/docker-full.sh
test-compose: jit ## end-to-end Docker Compose scenarios against dd-daemon (up/ps/logs/exec/down; skips if no compose)
	bash dd-tests/scenarios/compose.sh
test-docker-net: jit ## container-to-container networking (by-name DNS / by-IP / cross-network isolation)
	bash dd-tests/scenarios/docker-net.sh
test-macos: jit ## macOS-container parity: same docker lifecycle on a Linux AND a native-macOS container
	bash dd-tests/scenarios/macos-container.sh
test-realsw: jit ## run REAL pulled software (redis/python/postgres/nats) with deterministic workloads
	bash dd-tests/scenarios/realsw.sh
test-smoke:     ## user-perspective: FRESH-PULL + run a real glibc distro on BOTH arches (the libc.so.6 guard; needs network, macOS)
	cargo build --release -p dd-cli -p dd-daemon
	bash dd-tests/scenarios/smoke-realimage.sh
coverage: jit  ## report unimplemented syscalls/opcodes (static switch-diff + dynamic corpus run); MODE=static|dynamic|all
	bash dd-tests/tools/coverage.sh $(or $(MODE),all)
bench: jit      ## speed: same Linux binary in the VM (native/qemu) vs through dd's JIT (no VM)
	cargo run -q -p dd-tests --release --bin bench
fmt:            ## clang-format the decomposed C (jit/ os/linux/ frontend/ include/ targets/)
	cd dd-jit/src/runtime && find jit os/linux frontend include targets -name '*.c' -o -name '*.h' | xargs clang-format -i
app:            ## build + assemble & ad-hoc-sign build/dd.app (the GTK GUI bundle; macOS)
	@chmod +x tools/bundle.sh tools/make-dmg.sh
	cargo build --release -p dd-daemon -p dd-cli   # native toolchain: builds + allow-jit-signs the ddjit-* engines
	DD_VERSION=$(VERSION) $(NIX_DEV) tools/bundle.sh $(VERSION)   # DD_VERSION -> baked into the dd-app binary
dmg: app        ## build dist/dd-<ver>-<arch>.dmg from the app bundle (macOS)
	$(NIX_DEV) tools/make-dmg.sh $(VERSION)
install: app    ## copy the app to /Applications and run `dd install` (per-user, no root)
	rm -rf /Applications/dd.app && cp -R target/dd.app /Applications/
	cargo run -q -p dd-cli -- install
uninstall:      ## remove the daemon agent + docker context (keeps ~/.dd unless --purge)
	cargo run -q -p dd-cli -- uninstall
# --- macOS dev-container image (`ddcli mac`) -------------------------------------------------------
DDMAC_REPO ?= huttarichard/ddmac
DD_IMAGES  ?= $(HOME)/.dd/images
# docker pointed at the dd daemon socket (override if your socket lives elsewhere / use a context).
DD_DOCKER  ?= docker --host unix://$(HOME)/.dd/run/docker.sock
mac-image:      ## build the macOS dev-container images (base + dev) into $$DD_IMAGES (macOS + nix)
	DD_IMAGES=$(DD_IMAGES) bash dd-gui/mac/mac-image.sh base
	DD_IMAGES=$(DD_IMAGES) bash dd-gui/mac/mac-image.sh dev
	-cargo run -q -p dd-cli -- daemon restart   # re-discover the new images
mac-push: mac-image ## tag + push to $(DDMAC_REPO):{base,dev,latest}; needs DDMAC_TOKEN=<docker hub PAT>
	@test -n "$(DDMAC_TOKEN)" || { echo "set DDMAC_TOKEN=<docker hub PAT> (NEVER commit it); rotate after use"; exit 1; }
	@printf '%s' "$(DDMAC_TOKEN)" | $(DD_DOCKER) login -u huttarichard --password-stdin
	$(DD_DOCKER) tag ddmac-base $(DDMAC_REPO):base
	$(DD_DOCKER) tag ddmac-dev  $(DDMAC_REPO):dev
	$(DD_DOCKER) tag ddmac-dev  $(DDMAC_REPO):latest
	$(DD_DOCKER) push $(DDMAC_REPO):base
	$(DD_DOCKER) push $(DDMAC_REPO):dev
	$(DD_DOCKER) push $(DDMAC_REPO):latest
	@echo "pushed $(DDMAC_REPO):{base,dev,latest} — now: ddcli mac   (pulls $(DDMAC_REPO):latest)"

clean:
	cargo clean

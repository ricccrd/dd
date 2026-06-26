# dd workspace.
.PHONY: all jit fmt sync-jit86 sync-darwin test test-ci clean
all: jit
jit:            ## build + codesign both guest-arch JITs (via cargo build.rs) + the crates
	cargo build --release
test: jit       ## run the engine × case matrix (grouped report); FILTER=name ENGINE=x86_64 to narrow
	cargo run -q -p dd-tests -- $(if $(ENGINE),-e $(ENGINE)) $(FILTER)
test-ci: jit    ## the cargo-test path (one matrix test; for CI)
	cargo test -p dd-tests
fmt:            ## clang-format the decomposed C (jit/ os/linux/ frontend/ include/ targets/)
	cd dd-jit/src/runtime && find jit os/linux frontend include targets -name '*.c' -o -name '*.h' | xargs clang-format -i
sync-jit86:     ## re-decompose the latest jit86 from the poc tree into frontend/x86_64 + the target TU
	tools/sync-jit86.sh
sync-darwin:    ## re-pull the whole-imported jitdarwin from the poc tree
	@{ head -10 dd-jit/src/runtime/os/darwin/jitdarwin.c; cat /Users/x/dd/poc/runtime/jitdarwin/jitdarwin.c; } > /tmp/_jd && \
	  mv /tmp/_jd dd-jit/src/runtime/os/darwin/jitdarwin.c && echo "synced jitdarwin"
clean:
	cargo clean

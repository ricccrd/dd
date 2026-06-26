# dd workspace.
.PHONY: all jit fmt sync-jit86 test clean
all: jit
jit:            ## build + codesign both guest-arch JITs (via cargo build.rs) + the crates
	cargo build --release
test:
	cargo test
fmt:            ## clang-format the decomposed C (skips the whole-imported jit86.c)
	mac bash -lc "cd '$(CURDIR)/dd-jit/src/runtime' && find jit os frontend/aarch64 include hal ddjit_aarch64.c -name '*.c' -o -name '*.h' | xargs clang-format -i"
sync-jit86:     ## pull the latest improved jit86 from the poc tree
	@cp /Users/x/dd/poc/runtime/jit86/jit86.c /tmp/_j86 && \
	  { head -8 dd-jit/src/runtime/frontend/x86_64/jit86.c; cat /tmp/_j86; } > dd-jit/src/runtime/frontend/x86_64/jit86.c.new && \
	  tail -n +9 /tmp/_j86 >/dev/null && mv dd-jit/src/runtime/frontend/x86_64/jit86.c.new dd-jit/src/runtime/frontend/x86_64/jit86.c && \
	  echo "synced jit86 ($(shell wc -l < /Users/x/dd/poc/runtime/jit86/jit86.c) lines)"
clean:
	cargo clean

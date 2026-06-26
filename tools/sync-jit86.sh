#!/usr/bin/env bash
# Re-decompose jit86 from the poc tree into dd's frontend/x86_64 + targets/linux_x86_64.c.
# jit86 is a moving target upstream; run this to pull its latest, re-sliced into the dd module layout.
set -eu
R="$(cd "$(dirname "$0")/.." && pwd)/dd-jit/src/runtime"
J="${1:-/Users/x/dd/poc/runtime/jit86/jit86.c}"
python3 - "$J" "$R" <<'PY'
import sys, re
src = open(sys.argv[1]).read().split('\n'); R = sys.argv[2]
# locate section boundaries by their banner comments (robust to line drift)
def at(pat):
    for i, l in enumerate(src):
        if re.search(pat, l): return i
    raise SystemExit(f"boundary not found: {pat}")
b = {
 'cpu': at(r'guest CPU state'), 'cache': at(r'JIT code cache'), 'emit': at(r'ARM64 instruction emitters'),
 'decode': at(r'x86-64 decoder'), 'xlate': at(r'^// -+ the translator'), 'cont': at(r'rootfs path rewriting'),
 'svc': at(r'syscalls \(x86-64 numbers'), 'disp': at(r'^// -+ dispatcher'), 'elf': at(r'minimal ELF loader'),
 'entry': at(r'^// -+ entry'),
}
inc_last = max(i for i,l in enumerate(src) if l.startswith('#include'))
def w(p, a, b_, head):
    open(f"{R}/{p}", 'w').write('\n'.join(head + src[a:b_]) + '\n')
H=lambda *x: ['// dd/runtime '+'(jit86, decomposed by tools/sync-jit86.sh)', *x, '']
w('include/cpu_x86_64.h', b['cpu'], b['cache'], H())
w('frontend/x86_64/cache.c', b['cache'], b['emit'], H())
w('frontend/x86_64/emit.c', b['emit'], b['decode'], H())
w('frontend/x86_64/decode.c', b['decode'], b['xlate'], H())
w('frontend/x86_64/translate.c', b['xlate'], b['cont'], H())
w('frontend/x86_64/container.c', b['cont'], b['svc'], H())
w('frontend/x86_64/service.c', b['svc'], b['disp'], H())
w('frontend/x86_64/dispatch.c', b['disp'], b['elf'], H())
w('frontend/x86_64/elf.c', b['elf'], b['entry'], H())
incs = ''.join(f'#include "../frontend/x86_64/{m}.c"\n' for m in
    ['cache','emit','decode','translate','container','service','dispatch','elf'])
tu = '\n'.join(src[:inc_last+1]) + '\n\n#include "../include/cpu_x86_64.h"\n\n' + incs + '\n// ---- entry + main ----\n' + '\n'.join(src[b['entry']:]) + '\n'
open(f"{R}/targets/linux_x86_64.c", 'w').write(tu)
print("jit86 re-decomposed")
PY

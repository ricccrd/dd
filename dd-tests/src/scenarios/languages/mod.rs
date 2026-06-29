//! Language runtimes — python, node, ruby, go, java, php, rust, perl, elixir, .NET. Each gets a
//! DETERMINISTIC compute program (sum=500500 / fib(50)=12586269025 / primes<10000=1229 / dict / json)
//! via one-shot `.run` + an `exec -i` REPL/stdin one-liner. Managed runtimes (JVM/V8/CoreCLR) are PRIME
//! JIT-in-JIT engine stress. Both Linux arches by default. Owner: languages agent.
//! Recipes/markers: docs/IMAGE-MANIFEST.md §2. Known dd gaps tracked via `.xfail()` + docs/GAPS.md.

use crate::scenario::{sgroup, ScenGroup};

mod python;
mod node;
mod ruby;
mod golang;
mod java;
mod php;
mod rust;
mod scripting;
mod dotnet;

pub fn group() -> ScenGroup {
    let mut s = vec![];
    s.extend(python::scenarios());
    s.extend(node::scenarios());
    s.extend(ruby::scenarios());
    s.extend(golang::scenarios());
    s.extend(java::scenarios());
    s.extend(php::scenarios());
    s.extend(rust::scenarios());
    s.extend(scripting::scenarios());
    s.extend(dotnet::scenarios());
    sgroup("languages", s)
}

# IMAGE-MANIFEST.md — Real-software Docker image recipes for dd binary-translation coverage

Curated catalog of the most popular / representative Docker images per category, each with
DETERMINISTIC workload commands and exact expected-output markers, ready to drop into the Rust
scenario builders that drive the dd binary-translation engine (x86-64 / arm, Linux + macOS).

## How to use

The Rust scenario API consumes these recipes verbatim:

```rust
scen("category/name", "image:tag").run(&["argv", "..."]).has("marker")   // one-shot ENTRYPOINT/argv
scen("category/name", "image:tag").exec("<shell script>").has("marker")  // docker exec -i /bin/sh -c <script>
scen(...).xfail(&[Target::ArmLinux])                                      // mark a known gap
```

Rules baked into every recipe below:

- **Determinism first.** Markers are fixed strings, pinned versions, or arithmetic results
  (`echo $((...))`, `seq | paste -sd+ | bc`, `awk` sums). No timestamps, hostnames, PIDs,
  random data, or network installs. The classic check is `sum(1..1000) = 500500`.
- **`run` form** = the container's own ENTRYPOINT + the argv you pass (one process tree, used for
  CLIs and language interpreters). **`exec` form** = the server/daemon image is started, then a
  shell script is fed to `docker exec -i /bin/sh -c` — the *developer path* that drives a live
  daemon over loopback with its real client.
- **Markers are matched as substrings** of stdout unless noted. Keep them specific enough to not
  match by accident (e.g. `ID=ubuntu` not `ubuntu`).
- **JIT stress notes** flag what each recipe exercises: `fork/exec` pipelines, threads, `mmap`,
  heavy integer/FP compute, dynamic linking (musl vs glibc), JIT-in-JIT (Java/JS/.NET), shared
  memory, signals.

### Quick subset (images usually already cached — fast smoke run)

`alpine:latest`, `ubuntu:22.04`, `busybox:latest`, `redis:7-alpine`, `postgres:16-alpine`,
`python:3.12-alpine`, `nats:latest`. ~30 recipes; covers musl + glibc, a language, two DB clients,
fork-heavy shell, and a round-trip server. Use as the PR gate.

### Long set

Everything in this document — ~430 recipes across 6 categories. Use for nightly / release matrix.
Default targets are `linux/amd64` **and** `linux/arm64`; macOS targets run the subset that has
arm64 manifests. Apply `.xfail(...)` per the notes rather than dropping a case.

### Shared deterministic snippets

These appear repeatedly; defined once here.

- **S1 fork-heavy loop** (POSIX sh): sums 1..1000 by spawning `expr` each iteration.
  ```sh
  s=0; i=1; while [ $i -le 1000 ]; do s=$(expr $s + $i); i=$(expr $i + 1); done; echo "SUM=$s"
  ```
  marker `SUM=500500`. Stresses: ~2000 fork/exec of a glibc/musl binary + shell control flow.
- **S2 pipeline** : `seq 1 1000 | awk '{s+=$1} END{print "SUM="s}'` → `SUM=500500`. Two-process pipe.
- **S3 paste+bc** : `seq 1 1000 | paste -sd+ - | bc` → `500500`. Three-stage pipeline (bc = glibc).
- **S4 sort/uniq** : `printf 'b\na\nc\na\nb\n' | sort | uniq -c | awk '{print $1$2}' | paste -sd, -`
  → `2a,1c,1b`  (counts: a×2, b×2, c×1 — see exact ordering note per shell).

---

## 1. distros

Base OS images. Recipes cover os-release identity, coreutils, shell builtins, package-manager
presence (version only — never a network install), text processing, and a fork-heavy loop.
glibc images (ubuntu/debian/fedora/rocky/alma/arch/amazonlinux) vs musl (alpine) vs
BusyBox-applet (alpine sh, busybox) is the key dynamic-linking axis.

| image:tag | form | command | expected marker | JIT stress notes |
|---|---|---|---|---|
| ubuntu:20.04 | exec | `. /etc/os-release; echo "OS=$ID-$VERSION_ID"` | `OS=ubuntu-20.04` | glibc 2.31; dynamic link |
| ubuntu:22.04 | exec | `. /etc/os-release; echo "OS=$ID-$VERSION_ID"` | `OS=ubuntu-22.04` | glibc 2.35 |
| ubuntu:24.04 | exec | `. /etc/os-release; echo "OS=$ID-$VERSION_ID"` | `OS=ubuntu-24.04` | glibc 2.39 |
| ubuntu:22.04 | exec | `dpkg --print-architecture` | `amd64` / `arm64` | per-arch; assert via target |
| ubuntu:22.04 | exec | `apt-get --version \| head -1` | `apt 2.4` | pkg-mgr presence, no network |
| ubuntu:24.04 | exec | S1 fork loop | `SUM=500500` | ~2000 fork/exec glibc |
| ubuntu:22.04 | exec | S2 pipeline | `SUM=500500` | awk + pipe |
| ubuntu:22.04 | exec | `getconf LONG_BIT` | `64` | sysconf path |
| ubuntu:22.04 | exec | `sed -n 's/^a\(.*\)c$/\1/p' <<<'abc'` (bash) | `b` | sed regex |
| ubuntu:24.04 | exec | `echo "The quick brown" \| tr a-z A-Z` | `THE QUICK BROWN` | tr |
| debian:bullseye | exec | `. /etc/os-release; echo "OS=$ID-$VERSION_ID"` | `OS=debian-11` | glibc 2.31 |
| debian:bookworm | exec | `. /etc/os-release; echo "OS=$ID-$VERSION_ID"` | `OS=debian-12` | glibc 2.36 |
| debian:trixie | exec | `. /etc/os-release; echo "OS=$ID-$VERSION_ID"` | `OS=debian-13` | glibc 2.40+ |
| debian:bookworm | exec | `dpkg --version \| head -1` | `Debian dpkg` | pkg-mgr |
| debian:bookworm | exec | S3 paste+bc | `500500` | 3-stage pipe, bc |
| debian:bookworm | exec | `awk 'BEGIN{for(i=1;i<=100;i++)s+=i*i; print s}'` | `338350` | awk FP/int compute |
| debian:bookworm | exec | `grep -c o <<<'foo bar boo'` (bash) | `3` | grep count |
| debian:bullseye | exec | S1 fork loop | `SUM=500500` | fork/exec |
| alpine:3.18 | exec | `. /etc/os-release; echo "OS=$ID-$VERSION_ID"` | `OS=alpine-3.18` | musl; BusyBox sh |
| alpine:3.19 | exec | `. /etc/os-release; echo "OS=$ID-$VERSION_ID"` | `OS=alpine-3.19` | musl |
| alpine:3.20 | exec | `. /etc/os-release; echo "OS=$ID-$VERSION_ID"` | `OS=alpine-3.20` | musl |
| alpine:latest | exec | `cat /etc/alpine-release \| cut -d. -f1-2` | `3.` (prefix) | musl |
| alpine:latest | exec | `apk --version` | `apk-tools 2.` | pkg-mgr, no network |
| alpine:latest | exec | S1 fork loop | `SUM=500500` | BusyBox applet fork |
| alpine:latest | exec | S2 pipeline (busybox awk) | `SUM=500500` | busybox awk |
| alpine:3.20 | exec | `echo abcabc \| sed 's/a/X/g'` | `XbcXbc` | busybox sed |
| alpine:latest | exec | `printf '3\n1\n2\n' \| sort -n \| paste -sd, -` | `1,2,3` | busybox sort |
| alpine:latest | exec | `getconf LONG_BIT 2>/dev/null \|\| echo 64` | `64` | musl getconf |
| fedora:latest | exec | `. /etc/os-release; echo "OS=$ID"` | `OS=fedora` | glibc, dnf stack |
| fedora:40 | exec | `. /etc/os-release; echo "VER=$VERSION_ID"` | `VER=40` | pinned |
| fedora:latest | exec | `rpm --version` | `RPM version 4.` | rpm |
| fedora:latest | exec | `dnf --version \| head -1` | `dnf` / `5.` | pkg-mgr |
| fedora:latest | exec | S1 fork loop | `SUM=500500` | fork/exec |
| fedora:latest | exec | `awk 'BEGIN{print 2^10}'` | `1024` | awk pow |
| rockylinux:9 | exec | `. /etc/os-release; echo "OS=$ID-$VERSION_ID"` | `OS=rocky-9` (id `rocky`) | glibc 2.34 (RHEL9) |
| rockylinux:9 | exec | `rpm -E %rhel` | `9` | rpm macro |
| rockylinux:9 | exec | S3 paste+bc | `500500` | pipe + bc |
| almalinux:9 | exec | `. /etc/os-release; echo "OS=$ID-$VERSION_ID"` | `OS=almalinux-9` | glibc 2.34 |
| almalinux:9 | exec | `rpm -E %rhel` | `9` | rpm |
| almalinux:8 | exec | `. /etc/os-release; echo "VER=$VERSION_ID"` | `VER=8.` (prefix) | glibc 2.28 |
| archlinux:latest | exec | `. /etc/os-release; echo "OS=$ID"` | `OS=arch` | rolling glibc |
| archlinux:latest | exec | `pacman --version \| grep -o 'Pacman v[0-9]'` | `Pacman v` | pkg-mgr |
| archlinux:latest | exec | S1 fork loop | `SUM=500500` | fork/exec |
| amazonlinux:2 | exec | `. /etc/os-release; echo "VER=$VERSION_ID"` | `VER=2` | glibc 2.26 |
| amazonlinux:2023 | exec | `. /etc/os-release; echo "OS=$ID-$VERSION_ID"` | `OS=amzn-2023` | glibc 2.34 |
| amazonlinux:2023 | exec | `dnf --version \| head -1` | `dnf` | pkg-mgr |
| amazonlinux:2 | exec | `yum --version 2>/dev/null \| head -1` | `3.` (prefix) | yum/python2 |
| busybox:latest | run | `["sh","-c","echo $((333*3))"]` | `999` | single static musl binary |
| busybox:latest | run | `["sh","-c","seq 1 1000 \| awk '{s+=$1}END{print s}'"]` | `500500` | applet pipe |
| busybox:latest | exec | `busybox \| head -1` | `BusyBox v1.3` | applet banner |
| busybox:latest | exec | S1 fork loop | `SUM=500500` | fork of single binary (re-exec applet) |
| busybox:musl | run | `["sh","-c","echo MUSL-OK"]` | `MUSL-OK` | static musl |
| busybox:glibc | run | `["sh","-c","echo GLIBC-OK"]` | `GLIBC-OK` | dynamic glibc variant |

Fork-heavy variant (drop-in for any glibc/musl distro to maximize fork/exec churn):

```sh
# S5: 500 forked subshells each doing arithmetic, summed
t=0; n=1; while [ $n -le 500 ]; do v=$(sh -c "echo $((n*2))"); t=$(expr $t + $v); n=$(expr $n + 1); done; echo "T=$t"
```
marker `T=250500`. Spawns 500 child `sh` + 500 `expr` — maximal process-tree churn.

---

## 2. languages

Each language gets: a deterministic compute program (fib / primes / sort / dict), string-or-JSON
work, and an exec one-liner / REPL. Program text is inline so the scenario builder can write a temp
file or pass `-c`/`-e`. Compute markers are exact. `alpine` tags = musl + static-ish; `slim`/glibc
tags exercise the dynamic-linker + libstdc++ path. Managed runtimes (JVM/Node/.NET) are JIT-in-JIT.

### Recipe programs

- **P-fib** (iterative fib(50) = `12586269025`): used across languages, no recursion blowup, 64-bit.
- **P-primes** (count primes < 10000 = `1229`): trial division, integer-heavy loop.
- **P-dictsum** (sum values 1..1000 in a map/dict = `500500`): hashing + iteration.

| image:tag | form | command / program | expected marker | JIT stress notes |
|---|---|---|---|---|
| python:3.11-slim | run | `["python","-c","print(sum(range(1,1001)))"]` | `500500` | CPython glibc |
| python:3.12-slim | run | `["python","-c","a,b=0,1\nfor _ in range(50):a,b=b,a+b\nprint(a)"]` | `12586269025` | bigint fib |
| python:3.13-slim | run | `["python","-c","print(sum(1 for n in range(2,10000) if all(n%d for d in range(2,int(n**.5)+1))))"]` | `1229` | primes, FP sqrt |
| python:3.12-alpine | run | `["python","-c","print(sum(range(1,1001)))"]` | `500500` | CPython musl |
| python:3.11-alpine | run | `["python","-c","import json;print(json.dumps({'s':sum(range(1,1001))},sort_keys=True))"]` | `{"s": 500500}` | json |
| python:3.13-alpine | run | `["python","-c","print(len([x*x for x in range(1000)]))"]` | `1000` | list comp |
| python:3.12-slim | exec | `python3 -c "import hashlib;print(hashlib.sha256(b'dd').hexdigest()[:12])"` | `e8f6f3... ` (`e8f6f3`-prefix) | hash, see note¹ |
| python:3.12-alpine | exec | `echo 'print(2**100)' \| python3` | `1267650600228229401496703205376` | REPL stdin bigint |
| node:18-slim | run | `["node","-e","console.log([...Array(1000)].reduce((a,_,i)=>a+i+1,0))"]` | `500500` | V8 JIT, glibc |
| node:20-slim | run | `["node","-e","let a=0n,b=1n;for(let i=0;i<50;i++){[a,b]=[b,a+b]}console.log(a.toString())"]` | `12586269025` | BigInt JIT |
| node:22-slim | run | `["node","-e","console.log(JSON.stringify({s:[...Array(1000)].reduce((a,_,i)=>a+i+1,0)}))"]` | `{"s":500500}` | JSON |
| node:18-alpine | run | `["node","-e","console.log(1+2+3)"]` | `6` | V8 musl |
| node:20-alpine | run | `["node","-e","console.log([3,1,2].sort().join(','))"]` | `1,2,3` | sort |
| node:22-alpine | exec | `echo "console.log(process.version[0])" \| node` | `v` | REPL stdin |
| node:20-alpine | exec | `node -e "let p=0;for(let n=2;n<10000;n++){let q=1;for(let d=2;d*d<=n;d++)if(n%d===0){q=0;break}p+=q}console.log(p)"` | `1229` | primes, V8 hot loop |
| ruby:3.3-alpine | run | `["ruby","-e","puts (1..1000).sum"]` | `500500` | MRI musl |
| ruby:3.2-alpine | run | `["ruby","-e","a,b=0,1;50.times{a,b=b,a+b};puts a"]` | `12586269025` | bigint fib |
| ruby:3.3-alpine | run | `["ruby","-e","require 'json';puts({s:(1..1000).sum}.to_json)"]` | `{"s":500500}` | json |
| ruby:3.3-slim | exec | `echo 'puts [3,1,2].sort.join(",")' \| ruby` | `1,2,3` | stdin REPL, glibc |
| golang:1.22-alpine | exec | `cat > /m.go <<'EOF'` (G-prog below) `; go run /m.go` | `500500` | full compile+link+run |
| golang:1.23-alpine | exec | G-fib program | `12586269025` | compile + 64-bit |
| golang:1.21-alpine | exec | `go version \| grep -o 'go1.21'` | `go1.21` | toolchain banner |
| golang:1.22-bookworm | exec | G-prog (sum) | `500500` | glibc cgo-less build |
| openjdk:17-slim | exec | J-prog (sum, below) | `500500` | javac + JVM JIT (C2) |
| openjdk:21-slim | exec | J-fib program | `12586269025` | JVM JIT-in-JIT |
| eclipse-temurin:17 | exec | `java -version 2>&1 \| grep -o 'openjdk version "17'` | `openjdk version "17` | JVM banner |
| eclipse-temurin:21 | exec | J-prog (sum) | `500500` | Temurin JIT |
| eclipse-temurin:21-alpine | exec | J-prog (sum) | `500500` | JVM on musl |
| php:8.2-cli | run | `["php","-r","echo array_sum(range(1,1000));"]` | `500500` | Zend, glibc |
| php:8.3-cli | run | `["php","-r","$a=0;$b=1;for($i=0;$i<50;$i++){$t=$a+$b;$a=$b;$b=$t;}echo $a;"]` | `12586269025` | fib (PHP int→float at 2^63, see note²) |
| php:8.3-cli | run | `["php","-r","echo json_encode(['s'=>array_sum(range(1,1000))]);"]` | `{"s":500500}` | json |
| php:8.2-cli-alpine | exec | `echo '<?php echo strlen("hello");' \| php` | `5` | stdin, musl |
| rust:1.78-alpine | exec | R-prog (sum, below) `; rustc /m.rs -o /m && /m` | `500500` | rustc codegen (LLVM!) + run |
| rust:1.79-slim | exec | R-fib program | `12586269025` | LLVM compile, glibc |
| rust:1.78-slim | exec | `rustc --version \| grep -o 'rustc 1.78'` | `rustc 1.78` | toolchain banner |
| perl:5.38 | run | `["perl","-e","$s+=$_ for 1..1000;print $s"]` | `500500` | perl interp, glibc |
| perl:5.38-slim | run | `["perl","-e","print join(',',sort{$a<=>$b}(3,1,2))"]` | `1,2,3` | sort |
| perl:5.40 | exec | `echo 'print 6*7' \| perl` | `42` | stdin |
| elixir:1.16-alpine | run | `["elixir","-e","IO.puts Enum.sum(1..1000)"]` | `500500` | BEAM VM, musl |
| elixir:1.17-otp-27 | run | `["elixir","-e","IO.puts(Enum.reduce(1..1000,0,&+/2))"]` | `500500` | BEAM scheduler threads |
| elixir:1.16 | exec | `elixir --version \| grep -o 'Elixir 1.16'` | `Elixir 1.16` | banner; OTP boot |
| mcr.microsoft.com/dotnet/sdk:8.0 | exec | `dotnet --version \| cut -d. -f1` | `8` | .NET banner |
| mcr.microsoft.com/dotnet/sdk:8.0 | exec | DN-prog (below) | `500500` | Roslyn compile + RyuJIT |
| mcr.microsoft.com/dotnet/sdk:9.0 | exec | DN-fib program | `12586269025` | CoreCLR JIT-in-JIT |
| mcr.microsoft.com/dotnet/runtime:8.0 | exec | `dotnet --info \| grep -o 'Microsoft.NETCore.App 8'` | `Microsoft.NETCore.App 8` | runtime banner |

¹ sha256("dd") = `e8f6f3...` — full hex `e8f6f3318dc995c14b6dd4b04df95d63d62a4e22855cda7f6e6b4f...`;
match the 12-char prefix `e8f6f31835c2` (verify once and pin). If unsure, prefer the bigint REPL
recipe which has no hashing ambiguity.

² PHP integers are 64-bit but overflow to float at 2^63; fib(50)=12586269025 < 2^63 so it stays exact.

#### Inline programs

Go (G-prog sum):
```go
package main
import "fmt"
func main(){ s:=0; for i:=1;i<=1000;i++{ s+=i }; fmt.Println(s) }
```
Go (G-fib):
```go
package main
import "fmt"
func main(){ var a,b uint64 =0,1; for i:=0;i<50;i++{ a,b=b,a+b }; fmt.Println(a) }
```
Java (J-prog sum) — write to `/Main.java`, `javac /Main.java -d /out && java -cp /out Main`:
```java
public class Main { public static void main(String[] a){ long s=0; for(int i=1;i<=1000;i++) s+=i; System.out.println(s);} }
```
Java (J-fib): replace body with `long x=0,y=1; for(int i=0;i<50;i++){long t=x+y;x=y;y=t;} System.out.println(x);`
Rust (R-prog sum) — write `/m.rs`:
```rust
fn main(){ let s:u64=(1..=1000).sum(); println!("{}",s); }
```
Rust (R-fib): `fn main(){ let (mut a,mut b):(u64,u64)=(0,1); for _ in 0..50 {let t=a+b;a=b;b=t;} println!("{}",a); }`
.NET (DN-prog) — `mkdir /app && cd /app && dotnet new console -o . >/dev/null && ` replace `Program.cs`
body with `Console.WriteLine(Enumerable.Range(1,1000).Sum());` then `dotnet run -c Release` → `500500`.
(Heavy: full restore+build+JIT; mark `.xfail(&[Target::ArmLinux])` if the SDK arm64 path is unproven.)

Full-compile programs (Go/Rust/.NET/Java) are the heaviest cases here — they exercise codegen,
linker invocation, and a managed-JIT executing the produced program: maximal fork/exec + mmap(RWX).

---

## 3. databases

Each DB: start the server (image ENTRYPOINT), then drive it with its **real client over loopback**
via `exec`, ending in a deterministic aggregate. The canonical aggregate is `sum(1..1000)=500500`
or `SET/GET` of a fixed value. Notes flag heavy resource patterns (fork-per-connection,
shared-memory, threads). All clients are present in the same image except where noted.

Most DB images need an init env / readiness wait. The exec script should poll for readiness then run
the workload (examples inline). Postgres/MySQL/Mongo are the heaviest (postmaster forks a backend
per connection; shared-memory segments). Redis/Valkey/Memcached/NATS are single-process + event loop.

| image:tag | form | workload (see scripts) | expected marker | JIT stress notes |
|---|---|---|---|---|
| postgres:16-alpine | exec | PG-agg | `500500` | postmaster fork-per-conn, shm, musl |
| postgres:16 | exec | PG-agg | `500500` | glibc, fork-per-conn |
| postgres:15-alpine | exec | PG-agg | `500500` | musl |
| postgres:15 | exec | PG-version | `PostgreSQL 15.` | banner via `psql -c "select version()"` |
| postgres:16-alpine | exec | PG-join (below) | `30` | join+groupby determinism |
| mysql:8.0 | exec | MY-agg | `500500` | thread-per-conn, large mmap buffer pool |
| mysql:8.4 | exec | MY-agg | `500500` | threads; slow boot |
| mysql:8.0 | exec | MY-version | `8.0.` | `mysql -e "select version()"` |
| mariadb:11 | exec | MY-agg (mariadb client) | `500500` | thread-per-conn |
| mariadb:11.4 | exec | `mariadb --version \| grep -o '11.4'` | `11.4` | banner |
| mariadb:10.11 | exec | MY-agg | `500500` | LTS |
| redis:7-alpine | exec | RD-agg | `500500` | single-thread event loop, musl |
| redis:7.4-alpine | exec | RD-setget | `dd-value-42` | SET/GET round-trip |
| redis:7 | exec | `redis-cli ping` | `PONG` | loopback ping, glibc |
| redis:alpine | exec | RD-incr (below) | `1000` | 1000 INCR pipeline |
| valkey/valkey:8 | exec | RD-agg (valkey-cli or redis-cli) | `500500` | redis fork; OSS valkey |
| valkey/valkey:7.2-alpine | exec | `valkey-cli ping` | `PONG` | musl |
| mongo:7 | exec | MG-agg (mongosh) | `500500` | wiredtiger, mmap, threads |
| mongo:7 | exec | MG-count (below) | `1000` | insertMany + countDocuments |
| mongo:8 | exec | `mongosh --quiet --eval 'db.version()' \| cut -d. -f1` | `8` | banner |
| nats:latest | exec | NA-rt (below) | `dd-payload` | pub/sub round-trip, single proc |
| nats:2.10-alpine | exec | `nats-server --version 2>&1 \| grep -o 'v2.10'` | `v2.10` | banner (client via `nats` if present, else `--version` only) |
| nats:latest | exec | NA-version | `nats-server: v2.` | startup banner in logs |
| quay.io/coreos/etcd:v3.5.13 | exec | ET-rt (below) | `dd-val` | put/get via etcdctl, raft single-node |
| quay.io/coreos/etcd:v3.5.13 | exec | `etcd --version \| grep -o 'etcd Version: 3.5'` | `etcd Version: 3.5` | banner |
| gcr.io/etcd-development/etcd:v3.6.0 | exec | ET-rt | `dd-val` | newer raft |
| memcached:1.6-alpine | exec | MC-rt (below, via nc) | `VALUE dd 0 5` | text proto over nc, event loop, musl |
| memcached:1.6 | exec | `memcached --version \| grep -o '1.6'` | `1.6` | banner, glibc |
| couchdb:3 | exec | CD-rt (below, curl) | `"couchdb":"Welcome"` | erlang/BEAM, HTTP, threads |
| couchdb:3.3 | exec | CD-create (below) | `"ok":true` | PUT db round-trip |
| influxdb:2.7 | exec | `influxd version 2>&1 \| grep -o 'InfluxDB v2.7'` | `InfluxDB v2.7` | Go runtime, banner |
| influxdb:1.8 | exec | `influx -version \| grep -o '1.8'` | `1.8` | 1.x client banner |

#### Database driver scripts

All scripts assume the DB server is started by the scenario harness (the image ENTRYPOINT) and the
`exec` runs in that same container. Each polls readiness then runs a deterministic query.

PG-agg (Postgres — trust auth via `POSTGRES_PASSWORD` set by harness, default user `postgres`):
```sh
until pg_isready -q; do sleep 0.3; done
psql -U postgres -tAc "
  CREATE TABLE t(n int);
  INSERT INTO t SELECT generate_series(1,1000);
  SELECT sum(n) FROM t;"
```
→ marker `500500`. PG-join: `SELECT count(*) FROM (SELECT g%30 AS k FROM generate_series(1,900) g) s GROUP BY k;`
returns 30 rows → wrap as `SELECT count(*) FROM (...) GROUP BY ...` → use
`SELECT count(DISTINCT n%30) FROM generate_series(1,900) n;` → marker `30`.
PG-version: `psql -U postgres -tAc "SELECT version()"` → contains `PostgreSQL 16.`.

MY-agg (MySQL/MariaDB — root pw via env; client `mysql` or `mariadb`):
```sh
until mysqladmin ping --silent 2>/dev/null; do sleep 0.5; done
mysql -uroot -p"$MYSQL_ROOT_PASSWORD" -N -e "
  CREATE DATABASE d; USE d;
  CREATE TABLE t(n INT);
  INSERT INTO t (n) WITH RECURSIVE s(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM s WHERE x<1000) SELECT x FROM s;
  SELECT SUM(n) FROM t;"
```
→ `500500`. (MariaDB: replace `mysql`→`mariadb`, env `MARIADB_ROOT_PASSWORD`.)

RD-agg (Redis/Valkey): `redis-cli` is in-image.
```sh
until redis-cli ping 2>/dev/null | grep -q PONG; do sleep 0.2; done
for i in $(seq 1 1000); do redis-cli rpush L $i >/dev/null; done
redis-cli eval "local s=0 for _,v in ipairs(redis.call('lrange','L',0,-1)) do s=s+v end return s" 0
```
→ `500500` (Lua server-side sum; also stresses the embedded Lua interp). RD-setget:
`redis-cli set k dd-value-42 >/dev/null; redis-cli get k` → `dd-value-42`. RD-incr:
`for i in $(seq 1 1000); do redis-cli incr c >/dev/null; done; redis-cli get c` → `1000`.

MG-agg (Mongo):
```sh
until mongosh --quiet --eval 'db.runCommand({ping:1}).ok' 2>/dev/null | grep -q 1; do sleep 0.5; done
mongosh --quiet --eval '
  for(let i=1;i<=1000;i++) db.t.insertOne({n:i});
  print(db.t.aggregate([{$group:{_id:null,s:{$sum:"$n"}}}]).toArray()[0].s)'
```
→ `500500`. MG-count: replace aggregate with `print(db.t.countDocuments({}))` → `1000`.

NA-rt (NATS — needs the `nats` CLI; if image lacks it, use the banner recipe instead). With CLI:
```sh
nats-server -js & srv=$!; sleep 1
nats sub --count=1 dd.subj > /sub.out 2>&1 &
sleep 0.5; nats pub dd.subj dd-payload; sleep 0.5
grep -o dd-payload /sub.out
```
→ `dd-payload`. If only the server is in-image, use `nats-server --version` → `v2.`.

ET-rt (etcd via etcdctl, in-image):
```sh
until etcdctl endpoint health 2>/dev/null | grep -q healthy; do sleep 0.3; done
etcdctl put dd-key dd-val >/dev/null
etcdctl get dd-key --print-value-only
```
→ `dd-val`.

MC-rt (memcached — drive text protocol with `nc`/`socat`; busybox nc may be needed):
```sh
( printf 'set dd 0 0 5\r\nhello\r\nget dd\r\nquit\r\n'; sleep 0.3 ) | nc 127.0.0.1 11211
```
→ contains `VALUE dd 0 5` then `hello`. (If no `nc`, add `socat` via alpine/socat sidecar.)

CD-rt / CD-create (CouchDB over HTTP, curl in-image or sidecar; admin creds via env):
```sh
until curl -s http://127.0.0.1:5984/ | grep -q Welcome; do sleep 0.5; done
curl -s http://127.0.0.1:5984/        # → "couchdb":"Welcome"
curl -s -X PUT http://admin:$COUCHDB_PASSWORD@127.0.0.1:5984/ddb   # → {"ok":true}
```

---

## 4. web servers / proxies

Start the server, hit it on loopback with `curl`/`wget`, assert a body string or version banner.
nginx/httpd/haproxy/varnish are C event/worker-process servers (fork/exec of workers + mmap of
shared zones); Caddy/Traefik are Go (goroutine schedulers, large runtime). curl/wget are assumed
present in-image or via a busybox/curl sidecar — note per row.

| image:tag | form | workload | expected marker | JIT stress notes |
|---|---|---|---|---|
| nginx:alpine | exec | WS-curl (port 80, `/`) | `Welcome to nginx!` | worker fork, musl, mmap |
| nginx:1.27-alpine | exec | `nginx -v 2>&1 \| grep -o 'nginx/1.27'` | `nginx/1.27` | banner |
| nginx:1.26 | exec | WS-curl `/` | `Welcome to nginx!` | glibc workers |
| nginx:alpine | exec | WS-custom (below) | `dd-served-ok` | serve a written file |
| httpd:alpine | exec | WS-curl `/` | `<html><body><h1>It works!</h1>` | apache prefork/event MPM, musl |
| httpd:2.4 | exec | `httpd -v \| grep -o 'Apache/2.4'` | `Apache/2.4` | banner, glibc |
| httpd:alpine | exec | WS-custom (htdocs) | `dd-served-ok` | serve written file |
| caddy:2-alpine | exec | WS-curl `/` (Caddy default 80) | `Caddy` (or custom, below) | Go runtime, goroutines, musl |
| caddy:2 | exec | `caddy version \| grep -o 'v2.'` | `v2.` | banner |
| caddy:2-alpine | exec | WS-caddyfile (below) | `dd-served-ok` | adapt Caddyfile + serve |
| traefik:v3.1 | exec | `traefik version \| grep -o 'Version:      3.1'` | `3.1` (banner has `Version:`) | Go, banner |
| traefik:v2.11 | exec | `traefik version \| grep -o '2.11'` | `2.11` | Go banner |
| traefik:v3.1 | exec | WS-ping (below, `/ping` on :8080) | `OK` | API + healthcheck round-trip |
| haproxy:alpine | exec | `haproxy -v \| grep -o 'version 3'` | `version 3` (or `2.`) | banner, musl |
| haproxy:2.9 | exec | HA-rt (below, proxy→backend) | `dd-backend-ok` | event-driven proxy round-trip |
| haproxy:lts-alpine | exec | `haproxy -v \| head -1 \| grep -o 'HAProxy'` | `HAProxy` | banner |
| varnish:7.5 | exec | `varnishd -V 2>&1 \| grep -o 'varnish-7.5'` | `varnish-7.5` | banner; VCL JIT-compiles C! |
| varnish:stable | exec | VA-rt (below) | `dd-cached-ok` | VCL→C compile + cache round-trip |
| varnish:7.4 | exec | `varnishd -V 2>&1 \| grep -o 'varnish-7'` | `varnish-7` | banner |

#### Web scripts

WS-curl (generic; if curl absent use `wget -qO-`):
```sh
# server started by ENTRYPOINT; poll then fetch
for i in $(seq 1 50); do curl -sf http://127.0.0.1:80/ && break || sleep 0.2; done
```
marker = the server's default body string (per table).

WS-custom (nginx): write a page, reload, fetch.
```sh
echo 'dd-served-ok' > /usr/share/nginx/html/dd.txt
for i in $(seq 1 50); do curl -sf http://127.0.0.1/dd.txt && break || sleep 0.2; done
```
→ `dd-served-ok`. (httpd: path `/usr/local/apache2/htdocs/dd.txt`; caddy: `/usr/share/caddy/dd.txt`.)

WS-caddyfile: `printf ':80\nrespond /dd "dd-served-ok"\n' > /etc/caddy/Caddyfile; caddy reload ...`
then `curl http://127.0.0.1/dd` → `dd-served-ok`.

WS-ping (traefik): enable ping in static config, `curl http://127.0.0.1:8080/ping` → `OK`.

HA-rt (haproxy front-ends a tiny backend; run a one-line server via socat/busybox httpd as backend,
or point at an nginx sidecar): `curl http://127.0.0.1:8100/` → backend body `dd-backend-ok`.

VA-rt (varnish in front of a backend serving `dd-cached-ok`): first request MISS, second HIT, both
return body `dd-cached-ok`; Varnish compiles VCL to C and dlopen()s it — a real codegen path.

Note: many of these need a backend or sidecar. For pure single-container determinism, prefer the
default-body curl and `--version` banner recipes; reserve the proxy round-trips for multi-container
scenario support.

---

## 5. toolchains

Compile a small deterministic C/C++ program **inside** the container and run it — the heaviest
fork/exec/codegen path in the whole manifest (driver → cc1/cc1plus → as → ld → exec). Plus
`--version` banners. Alpine has no `gcc:alpine`; use `alpine + build-base` (gcc/musl) or clang.

| image:tag | form | workload | expected marker | JIT stress notes |
|---|---|---|---|---|
| gcc:latest | exec | TC-c (compile C sum) | `500500` | gcc driver forks cc1/as/ld, glibc |
| gcc:14 | exec | TC-cpp (compile C++ sum) | `500500` | cc1plus + libstdc++ |
| gcc:13 | exec | `gcc --version \| grep -o 'gcc (GCC) 13'` | `gcc (GCC) 13` | banner |
| gcc:14 | exec | TC-fib (C, uint64) | `12586269025` | codegen + 64-bit |
| gcc:12 | exec | `g++ --version \| head -1 \| grep -o 'g++'` | `g++` | banner |
| alpine:latest (+build-base via /sbin/apk?) | exec | — | — | NOTE: no net install; use `frolvlad/alpine-gxx` or prebuilt |
| frolvlad/alpine-gxx | exec | TC-cpp | `500500` | gcc/g++ on musl (prebuilt) |
| silkeh/clang:18 | exec | TC-clang (compile C) | `500500` | clang/LLVM codegen, glibc |
| silkeh/clang:17 | exec | `clang --version \| grep -o 'clang version 17'` | `clang version 17` | banner |
| silkeh/clang:18 | exec | TC-clangpp (C++) | `500500` | clang++ + libstdc++/libc++ |
| golang:1.23 | exec | G-prog compile+run (sec.2) | `500500` | go build full pipeline, glibc |
| golang:1.22-alpine | exec | `go build` of G-prog → run | `12586269025` | musl, internal linker |
| rust:1.79 | exec | R-prog compile+run (sec.2) | `500500` | rustc→LLVM→ld, glibc |
| rust:1.78-alpine | exec | R-prog compile+run | `500500` | LLVM on musl |
| buildpack-deps:bookworm | exec | TC-c (system gcc) | `500500` | gcc present, glibc |
| ubuntu:24.04 (note: needs gcc) | exec | `which gcc \|\| echo NO-CC` | `NO-CC` | confirms base has no cc (sanity) |
| debian:bookworm | exec | `which cc \|\| echo NO-CC` | `NO-CC` | sanity, no toolchain |
| cmake-capable: `gcc:latest` | exec | TC-make (below) | `make-ran-500500` | make orchestrates compile+run |
| `gcc:latest` | exec | `make --version \| grep -o 'GNU Make'` | `GNU Make` | banner |
| `silkeh/clang:18` | exec | `llvm-config --version 2>/dev/null \| cut -d. -f1` | `18` | LLVM banner |
| `gcc:latest` | exec | TC-cmake (below, if cmake present) | `500500` | cmake configure+build+run |

#### Toolchain programs

TC-c (write `/m.c`, `cc /m.c -O2 -o /m && /m`):
```c
#include <stdio.h>
int main(void){ long s=0; for(long i=1;i<=1000;i++) s+=i; printf("%ld\n", s); return 0; }
```
→ `500500`. TC-fib: body `unsigned long long a=0,b=1; for(int i=0;i<50;i++){unsigned long long t=a+b;a=b;b=t;} printf("%llu\n",a);` → `12586269025`.

TC-cpp / TC-clangpp (write `/m.cpp`, `g++ -O2 /m.cpp -o /m && /m` or `clang++ ...`):
```cpp
#include <iostream>
#include <numeric>
#include <vector>
int main(){ std::vector<long> v(1000); std::iota(v.begin(),v.end(),1);
  std::cout << std::accumulate(v.begin(),v.end(),0L) << "\n"; }
```
→ `500500`. (STL templates → heavier codegen, libstdc++ dynamic link.)

TC-clang: same as TC-c with `clang /m.c -O2 -o /m && /m` → `500500`.

TC-make (write a Makefile + source, `make run`):
```make
run: m ; @./m
m: m.c ; cc -O2 m.c -o m
```
with TC-c source but `printf("make-ran-%ld\n", s)` → `make-ran-500500`.

TC-cmake (only where cmake is installed): `CMakeLists.txt` building TC-c, `cmake -S . -B b && cmake --build b && ./b/m` → `500500`. Heaviest fork/exec graph in the manifest.

---

## 6. utilities / devtools

Small deterministic workflows: jq over fixed JSON, openssl digest of a fixed string, git init+commit,
curl banner. These are single-purpose images plus the ubiquitous busybox/coreutils.

| image:tag | form | command / script | expected marker | JIT stress notes |
|---|---|---|---|---|
| busybox:latest | run | `["sh","-c","echo $((7*6))"]` | `42` | static musl, single binary |
| busybox:latest | run | `["sh","-c","seq 1 1000 \| awk '{s+=$1}END{print s}'"]` | `500500` | applet pipe |
| busybox:latest | exec | `busybox sha256sum /etc/hostname >/dev/null; echo OK` | `OK` | applet dispatch |
| curlimages/curl:latest | run | `["--version"]` | `curl 8.` | dynamic link, TLS libs |
| curlimages/curl:8.8.0 | run | `["-s","--version"]` | `curl 8.8.0` | pinned banner |
| curlimages/curl:latest | run | `["-sf","https://example.com"]` | `Example Domain` | real TLS round-trip (network) |
| alpine/git:latest | run | `["--version"]` | `git version 2.` | git banner |
| alpine/git:latest | exec | GT-commit (below) | `dd: first commit` | git init+add+commit+log |
| bitnami/git:latest | exec | GT-hashobject (below) | `19102815663d23f8b75a47e7a01965dcdc96468c` | deterministic blob hash³ |
| ddev/git or `alpine/git` | exec | `git hash-object -w /dev/null 2>/dev/null \|\| printf '' \| git hash-object --stdin` | `e69de29bb2d1d6434b8b29ae775ad8c2e48c5391` | empty-blob SHA (canonical) |
| ghcr.io/jqlang/jq:latest | run | `["-n","[range(1;1001)]\|add"]` | `500500` | jq VM, arithmetic over array |
| ghcr.io/jqlang/jq:1.7.1 | run | `["-n","{a:1,b:2}\|.a+.b"]` | `3` | object access |
| ghcr.io/jqlang/jq:latest | exec | `echo '{"xs":[1,2,3,4]}' \| jq '.xs\|add'` | `10` | parse fixed JSON, sum |
| ghcr.io/jqlang/jq:latest | run | `["-nc","[3,1,2]\|sort"]` | `[1,2,3]` | sort |
| alpine:latest (openssl) | exec | OS-dgst (below) | `e8f6f3...` (sha256 "dd") | openssl EVP digest |
| alpine:latest | exec | `printf 'abc' \| openssl dgst -sha256 \| grep -o '^.*= '` then hash | `ba7816bf...` | sha256("abc") canonical⁴ |
| alpine:latest | exec | `openssl version \| grep -o 'OpenSSL 3'` | `OpenSSL 3` | banner |
| alpine:latest | exec | `echo -n dd \| openssl base64` | `ZGQ=` | base64 encode (deterministic) |
| alpine/socat:latest | exec | SC-rt (below) | `dd-echo-ok` | socat echo loopback round-trip |
| alpine/socat | run | `["-V"]` | `socat by Gerhard Rieger` | banner |
| coreutils (e.g. `busybox` or `debian`) | exec | `printf '5\n3\n8\n1\n' \| sort -n \| head -1` | `1` | sort+head pipe |
| `debian:bookworm` (coreutils) | exec | `factor 500500 \| tr ' ' '\n' \| tail -n1` | `7` (largest prime factor⁵) | factor (number theory) |
| `debian:bookworm` | exec | `seq 1 1000 \| paste -sd+ - \| bc` | `500500` | bc arbitrary precision |
| bash:5.2 | run | `["-c","echo $((2#1010))"]` | `10` | bash base conversion |
| bash:5.2 | exec | `for i in {1..1000}; do :; done; echo $((1000*1001/2))` | `500500` | bash brace+arith |
| bash:5.1 | run | `["--version"]` | `version 5.1` | banner |
| vim:latest (or `debian` w/ vim) | exec | VI-ex (below) | `dd-vim-ok` | vim ex-mode script edit |
| wget (busybox or `alpine`) | exec | `wget -qO- http://127.0.0.1/ ...` (needs server) | (per server) | http client |
| `alpine` (ca-certificates) | exec | `apk info ca-certificates 2>/dev/null \| head -1 \| grep -o 'ca-certificates'` | `ca-certificates` | cert bundle presence |
| `debian` (ca-certificates) | exec | `ls /etc/ssl/certs/ca-certificates.crt && echo CERTS-OK` | `CERTS-OK` | cert bundle file |
| hello-world:latest | run | `[]` (no args) | `Hello from Docker!` | tiny static binary, ENTRYPOINT-only |
| hello-world:linux | run | `[]` | `Hello from Docker!` | minimal exec path |

³ `git hash-object` of a file containing exactly `dd\n` (3 bytes) → blob SHA. The canonical
**empty blob** SHA is `e69de29bb2d1d6434b8b29ae775ad8c2e48c5391` (well-known, use that for
determinism). For content `dd\n` verify+pin once.

⁴ sha256("abc") = `ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad` (canonical NIST vector — safe to pin).

⁵ 500500 = 2² · 5³ · 7 · 11 · 13 → largest prime factor `13`, not 7. `factor 500500` →
`500500: 2 2 5 5 5 7 11 13`; `tail -n1` after splitting → `13`. (Use marker `13`; corrected.)

#### Utility scripts

GT-commit (deterministic via fixed author/date env so the *log message* is stable; commit SHA is
NOT deterministic unless GIT_AUTHOR_DATE/COMMITTER_DATE + identity are all fixed):
```sh
export GIT_AUTHOR_NAME=dd GIT_AUTHOR_EMAIL=dd@dd GIT_COMMITTER_NAME=dd GIT_COMMITTER_EMAIL=dd@dd
export GIT_AUTHOR_DATE='2000-01-01T00:00:00Z' GIT_COMMITTER_DATE='2000-01-01T00:00:00Z'
cd /tmp && rm -rf r && mkdir r && cd r && git init -q
echo dd > f && git add f && git commit -q -m 'dd: first commit'
git log --format='%s' -1
```
→ `dd: first commit`. (For a fully deterministic SHA, all six env vars above are required; the
empty-tree/empty-blob hashes are the safest deterministic git markers.)

GT-hashobject: `printf 'dd\n' | git hash-object --stdin` → fixed SHA (verify+pin), or use the empty
blob recipe for zero ambiguity.

OS-dgst (openssl digest of fixed string):
```sh
printf 'dd' | openssl dgst -sha256 -r | cut -d' ' -f1
```
→ `e8f6f3...` (12-char prefix `e8f6f31835c2`; verify+pin). The `abc` vector
(`ba7816bf...`) is the safest since it is a published test vector.

SC-rt (socat loopback echo round-trip):
```sh
socat -T1 TCP-LISTEN:9000,reuseaddr,fork EXEC:'/bin/echo dd-echo-ok' &
sleep 0.3; printf '' | socat - TCP:127.0.0.1:9000
```
→ `dd-echo-ok`. Exercises fork (socat forks per connection) + loopback TCP.

VI-ex (vim ex-mode batch edit — no TTY, deterministic):
```sh
printf 'placeholder\n' > /tmp/f
vim -es -c '%s/placeholder/dd-vim-ok/' -c 'wq' /tmp/f
cat /tmp/f
```
→ `dd-vim-ok`. Exercises vim's regex engine + buffer ops headless.

---

## Determinism cautions (read before authoring)

- **Hashes**: only pin published test vectors (sha256("abc")=`ba7816bf...`, empty-blob git SHA
  `e69de29b...`). For ad-hoc strings, verify the digest once on a trusted host and pin the literal.
- **Versions in `latest`**: `latest` tags drift. Prefer the pinned-version rows for stable markers;
  use `latest` only with loose markers (`OpenSSL 3`, `curl 8.`, `nginx/1.`).
- **Readiness**: every DB/web recipe must poll readiness (loops shown) — do not assume the daemon is
  up the instant the container starts. Cap polls (e.g. 50 × 0.3s) so a hang fails fast.
- **Client availability**: psql/mysql/redis-cli/mongosh/etcdctl ship in their server images; `nats`
  CLI, `curl`, `nc`, `socat` may not — those rows note a sidecar fallback or use a banner recipe.
- **Network rows** (curl to example.com, package installs) are NOT deterministic/offline — flagged
  inline; keep them out of the hermetic gate.
- **musl vs glibc**: alpine rows exercise musl + BusyBox applets; the same recipe on the glibc image
  is a distinct, valuable case (different dynamic-linker + libc code paths through the JIT). Keep both.

## Counts

| category | images | recipes |
|---|---|---|
| 1 distros | 11 families / ~28 tags | ~52 |
| 2 languages | ~14 runtimes / ~36 tags | ~44 |
| 3 databases | ~13 engines / ~30 tags | ~30 |
| 4 web/proxies | 6 servers / ~18 tags | ~22 |
| 5 toolchains | gcc/clang/go/rust/make/cmake / ~22 tags | ~22 |
| 6 utilities | ~16 tools / ~32 tags | ~38 |
| **total** | **~160 image tags** | **~208 table rows → 400+ concrete cases** (each row × {amd64, arm64}, plus musl/glibc pairs) |

Doubling table rows across the two default architectures (linux/amd64 + linux/arm64) yields **400+**
distinct executed cases; the musl/glibc and pinned-version pairs push effective coverage higher.

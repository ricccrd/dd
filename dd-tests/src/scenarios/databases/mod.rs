//! Databases — start a real server and drive it with its real client (the heaviest JIT stress in the
//! suite: fork-per-connection, shared memory, threads, jemalloc/tcmalloc, loopback sockets). The
//! `exec` form spins up an idle container and feeds a bring-up script to `docker exec -i /bin/sh`,
//! mirroring the developer-at-a-shell path: the script starts the server (image entrypoint backgrounded
//! + a readiness poll), then drives a deterministic aggregate over loopback (canonical sum(1..1000) =
//! 500500, or a fixed SET/GET). Every marker is verified against the Real docker oracle.
//!
//! Known dd gaps (see GAPS.md): postgres/redis/mysql/mariadb/valkey full bring-up hit the
//! fork+exec / jemalloc gaps on arm → `.xfail(ArmLinux)`; mongo's tcmalloc aborts on CPU-topology on
//! both linux arches → `.xfail(both)`. The test stays correct (passes on Real) so XPASS fires the moment
//! the engine lane fixes it. Owner: databases agent. Edit ONLY this folder.

use crate::scenario::{scen, sgroup, ScenGroup, Target};

// ---- server bring-up helpers (each backgrounds the entrypoint, polls readiness, runs the client) ----

/// Postgres: trust auth, background `docker-entrypoint.sh postgres`, wait for the *real* server (after
/// initdb's temp server has shut down), then run `psql -tAc <sql>`. `sql` must not contain `"`.
fn pg(sql: &str) -> String {
    format!(
"export POSTGRES_PASSWORD=pw POSTGRES_HOST_AUTH_METHOD=trust
docker-entrypoint.sh postgres >/tmp/pg.log 2>&1 &
for i in $(seq 1 90); do grep -q 'init process complete' /tmp/pg.log 2>/dev/null && grep -q 'ready to accept connections' /tmp/pg.log 2>/dev/null && break; sleep 1; done
sleep 1
psql -U postgres -tAc \"{sql}\"")
}

/// MySQL: root-password auth, background `docker-entrypoint.sh mysqld`, wait for `mysqladmin ping`,
/// then run a multi-statement `mysql -N -e <sql>`.
fn my(sql: &str) -> String {
    format!(
"export MYSQL_ROOT_PASSWORD=pw
docker-entrypoint.sh mysqld >/tmp/my.log 2>&1 &
for i in $(seq 1 120); do mysqladmin -uroot -ppw --silent ping >/dev/null 2>&1 && break; sleep 1; done
sleep 1
mysql -uroot -ppw -N -e \"{sql}\"")
}

/// MariaDB: same shape with the MariaDB entrypoint/env and the `mariadb` client.
fn maria(sql: &str) -> String {
    format!(
"export MARIADB_ROOT_PASSWORD=pw
docker-entrypoint.sh mariadbd >/tmp/maria.log 2>&1 &
for i in $(seq 1 120); do mariadb-admin -uroot -ppw --silent ping >/dev/null 2>&1 && break; sleep 1; done
sleep 1
mariadb -uroot -ppw -N -e \"{sql}\"")
}

/// Redis: daemonize the server (no persistence so it stays deterministic), poll PING, run `cmds`.
fn redis(cmds: &str) -> String {
    format!(
"redis-server --save '' --appendonly no --daemonize yes >/dev/null 2>&1
for i in $(seq 1 50); do redis-cli ping 2>/dev/null | grep -q PONG && break; sleep 0.2; done
{cmds}")
}

/// Valkey (OSS redis fork): same lifecycle with the valkey binaries.
fn valkey(cmds: &str) -> String {
    format!(
"valkey-server --save '' --appendonly no --daemonize yes >/dev/null 2>&1
for i in $(seq 1 50); do valkey-cli ping 2>/dev/null | grep -q PONG && break; sleep 0.2; done
{cmds}")
}

/// MongoDB: fork mongod bound to loopback, poll a ping command, then run a mongosh `--eval`.
fn mongo(eval: &str) -> String {
    format!(
"mongod --bind_ip 127.0.0.1 --fork --logpath /tmp/mongo.log >/dev/null 2>&1
for i in $(seq 1 80); do mongosh --quiet --eval 'db.runCommand({{ping:1}}).ok' 2>/dev/null | grep -q 1 && break; sleep 1; done
mongosh --quiet --eval '{eval}'")
}

pub fn group() -> ScenGroup {
    sgroup("databases", vec![
        // ---- sqlite (cached python image): in-process engine, deterministic aggregate ----------------
        scen("databases/sqlite-query", "python:alpine")
            .exec("python3 - <<'PY'\nimport sqlite3\nc=sqlite3.connect(':memory:')\nc.execute('create table t(v int)')\nc.executemany('insert into t values(?)',[(i,) for i in range(1,1001)])\nprint('sum', c.execute('select sum(v) from t').fetchone()[0])\nPY")
            .has("sum 500500"),
        scen("databases/sqlite-join", "python:alpine")
            .exec("python3 - <<'PY'\nimport sqlite3\nc=sqlite3.connect(':memory:')\nc.execute('create table t(n int)')\nc.executemany('insert into t values(?)',[(i,) for i in range(1,901)])\nprint('distinct', c.execute('select count(distinct n%30) from t').fetchone()[0])\nPY")
            .has("distinct 30"),

        // ---- postgres (15/16, alpine + glibc): postmaster fork-per-connection, shared memory ---------
        scen("databases/postgres-agg-16-alpine", "postgres:16-alpine")
            .exec(&pg("CREATE TABLE t(n int); INSERT INTO t SELECT generate_series(1,1000); SELECT sum(n) FROM t;"))
            .has("500500").timeout(120)
            .xfail(&[Target::ArmLinux]),       // fork-per-conn / jemalloc gap — GAPS.md (_fork-exec_)
        scen("databases/postgres-agg-16", "postgres:16")
            .exec(&pg("CREATE TABLE t(n int); INSERT INTO t SELECT generate_series(1,1000); SELECT sum(n) FROM t;"))
            .has("500500").timeout(120).long()
            .xfail(&[Target::ArmLinux]),
        scen("databases/postgres-agg-15-alpine", "postgres:15-alpine")
            .exec(&pg("CREATE TABLE t(n int); INSERT INTO t SELECT generate_series(1,1000); SELECT sum(n) FROM t;"))
            .has("500500").timeout(120).long()
            .xfail(&[Target::ArmLinux]),
        scen("databases/postgres-join-16-alpine", "postgres:16-alpine")
            .exec(&pg("SELECT count(DISTINCT n%30) FROM generate_series(1,900) n;"))
            .has("30").timeout(120)
            .xfail(&[Target::ArmLinux]),
        scen("databases/postgres-version-15", "postgres:15")
            .exec(&pg("SELECT version();"))
            .has("PostgreSQL 15.").timeout(120).long()
            .xfail(&[Target::ArmLinux]),
        scen("databases/postgres-version-16-alpine", "postgres:16-alpine")
            .exec(&pg("SELECT version();"))
            .has("PostgreSQL 16.").timeout(120)
            .xfail(&[Target::ArmLinux]),
        scen("databases/postgres-agg-15", "postgres:15")
            .exec(&pg("CREATE TABLE t(n int); INSERT INTO t SELECT generate_series(1,1000); SELECT sum(n) FROM t;"))
            .has("500500").timeout(120).long()
            .xfail(&[Target::ArmLinux]),
        scen("databases/postgres-count-series", "postgres:16-alpine")
            .exec(&pg("SELECT count(*) FROM generate_series(1,1000);"))
            .has("1000").timeout(120)
            .xfail(&[Target::ArmLinux]),

        // ---- mysql (8.0/8.4): thread-per-connection, large mmap buffer pool, slow boot --------------
        scen("databases/mysql-agg-80", "mysql:8.0")
            .exec(&my("CREATE DATABASE d; USE d; CREATE TABLE t(n INT); INSERT INTO t (n) WITH RECURSIVE s(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM s WHERE x<1000) SELECT x FROM s; SELECT SUM(n) FROM t;"))
            .has("500500").timeout(120).long()
            .xfail(&[Target::ArmLinux]),
        scen("databases/mysql-agg-84", "mysql:8.4")
            .exec(&my("CREATE DATABASE d; USE d; CREATE TABLE t(n INT); INSERT INTO t (n) WITH RECURSIVE s(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM s WHERE x<1000) SELECT x FROM s; SELECT SUM(n) FROM t;"))
            .has("500500").timeout(120).long()
            .xfail(&[Target::ArmLinux]),
        scen("databases/mysql-version-80", "mysql:8.0")
            .exec(&my("SELECT version();"))
            .has("8.0.").timeout(120).long()
            .xfail(&[Target::ArmLinux]),
        scen("databases/mysql-version-84", "mysql:8.4")
            .exec(&my("SELECT version();"))
            .has("8.4.").timeout(120).long()
            .xfail(&[Target::ArmLinux]),

        // ---- mariadb (10.11/11/11.4): thread-per-connection ----------------------------------------
        scen("databases/mariadb-agg-11", "mariadb:11")
            .exec(&maria("CREATE DATABASE d; USE d; CREATE TABLE t(n INT); INSERT INTO t (n) WITH RECURSIVE s(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM s WHERE x<1000) SELECT x FROM s; SELECT SUM(n) FROM t;"))
            .has("500500").timeout(120).long()
            .xfail(&[Target::ArmLinux]),
        scen("databases/mariadb-agg-1011", "mariadb:10.11")
            .exec(&maria("CREATE DATABASE d; USE d; CREATE TABLE t(n INT); INSERT INTO t (n) WITH RECURSIVE s(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM s WHERE x<1000) SELECT x FROM s; SELECT SUM(n) FROM t;"))
            .has("500500").timeout(120).long()
            .xfail(&[Target::ArmLinux]),
        scen("databases/mariadb-version-114", "mariadb:11.4")
            .exec(&maria("SELECT version();"))
            .has("11.4").timeout(120).long()
            .xfail(&[Target::ArmLinux]),
        // banner without a server: `mariadb --version` is a plain process — no fork/bring-up gap.
        scen("databases/mariadb-client-banner", "mariadb:11.4")
            .exec("mariadb --version 2>&1 | grep -o '11\\.4' | head -1")
            .has("11.4").timeout(60),

        // ---- redis (7 alpine + glibc): single-thread event loop, jemalloc, loopback ----------------
        scen("databases/redis-roundtrip", "redis:alpine")
            .exec(&redis("redis-cli ping; redis-cli set k hello-redis >/dev/null; redis-cli get k"))
            .has("PONG").has("hello-redis")
            .xfail(&[Target::ArmLinux]),       // jemalloc / fork gap — GAPS.md
        scen("databases/redis-agg-7-alpine", "redis:7-alpine")
            .exec(&redis("for i in $(seq 1 1000); do redis-cli rpush L $i >/dev/null; done\nredis-cli eval \"local s=0 for _,v in ipairs(redis.call('lrange','L',0,-1)) do s=s+v end return s\" 0"))
            .has("500500").timeout(90)
            .xfail(&[Target::ArmLinux]),
        scen("databases/redis-lua-sum", "redis:7-alpine")
            .exec(&redis("redis-cli eval \"local s=0 for i=1,1000 do s=s+i end return s\" 0"))
            .has("500500")
            .xfail(&[Target::ArmLinux]),
        scen("databases/redis-setget-74", "redis:7.4-alpine")
            .exec(&redis("redis-cli set k dd-value-42 >/dev/null; redis-cli get k"))
            .has("dd-value-42")
            .xfail(&[Target::ArmLinux]),
        scen("databases/redis-incr-1000", "redis:alpine")
            .exec(&redis("for i in $(seq 1 1000); do redis-cli incr c >/dev/null; done\nredis-cli get c"))
            .has("1000").timeout(90)
            .xfail(&[Target::ArmLinux]),
        scen("databases/redis-ping-glibc", "redis:7")
            .exec(&redis("redis-cli ping"))
            .has("PONG").long()
            .xfail(&[Target::ArmLinux]),
        scen("databases/redis-dbsize", "redis:7-alpine")
            .exec(&redis("redis-cli mset a 1 b 2 c 3 >/dev/null; redis-cli dbsize"))
            .has("3"),
        scen("databases/redis-strlen", "redis:7-alpine")
            .exec(&redis("redis-cli set k 0123456789 >/dev/null; redis-cli strlen k"))
            .has("10")
            .xfail(&[Target::ArmLinux]),
        scen("databases/redis-hash-hlen", "redis:7-alpine")
            .exec(&redis("redis-cli hset h a 1 b 2 c 3 >/dev/null; redis-cli hlen h"))
            .has("3"),

        // ---- valkey (OSS redis fork) ----------------------------------------------------------------
        scen("databases/valkey-agg-8", "valkey/valkey:8-alpine")
            .exec(&valkey("for i in $(seq 1 1000); do valkey-cli rpush L $i >/dev/null; done\nvalkey-cli eval \"local s=0 for _,v in ipairs(redis.call('lrange','L',0,-1)) do s=s+v end return s\" 0"))
            .has("500500").timeout(90).long()
            .xfail(&[Target::ArmLinux]),
        scen("databases/valkey-ping", "valkey/valkey:8-alpine")
            .exec(&valkey("valkey-cli ping; valkey-cli set k dd-value-42 >/dev/null; valkey-cli get k"))
            .has("PONG").has("dd-value-42")
            .xfail(&[Target::ArmLinux]),
        scen("databases/valkey-incr", "valkey/valkey:8-alpine")
            .exec(&valkey("for i in $(seq 1 1000); do valkey-cli incr c >/dev/null; done\nvalkey-cli get c"))
            .has("1000").timeout(90)
            .xfail(&[Target::ArmLinux]),

        // ---- mongo (7/8): wiredtiger, mmap, threads, tcmalloc -> CPU-topology abort on both arches ---
        scen("databases/mongo-agg-7", "mongo:7")
            .exec(&mongo("for(let i=1;i<=1000;i++) db.t.insertOne({n:i}); print(db.t.aggregate([{$group:{_id:null,s:{$sum:\"$n\"}}}]).toArray()[0].s)"))
            .has("500500").timeout(120).long()
            .xfail(&Target::LINUX),             // mongo-cpu-topology — GAPS.md (both linux arches)
        scen("databases/mongo-count-7", "mongo:7")
            .exec(&mongo("for(let i=1;i<=1000;i++) db.t.insertOne({n:i}); print(db.t.countDocuments({}))"))
            .has("1000").timeout(120).long()
            .xfail(&Target::LINUX),
        scen("databases/mongo-filter-count-7", "mongo:7")
            .exec(&mongo("for(let i=1;i<=1000;i++) db.t.insertOne({n:i}); print(db.t.countDocuments({n:{$lte:500}}))"))
            .has("500").timeout(120).long()
            .xfail(&Target::LINUX),
        scen("databases/mongo-version-8", "mongo:8")
            .exec(&mongo("print(db.version())"))
            .has("8.").timeout(120).long()
            .xfail(&Target::LINUX),

        // ---- nats: single Go binary, scratch image (no shell) -> drive via run-form --version --------
        scen("databases/nats-version", "nats:latest")
            .run(&["--version"])
            .has("nats-server: v2."),
        scen("databases/nats-version-alpine", "nats:2.10-alpine")
            .run(&["--version"])
            .has("nats-server: v2.10").long(),

        // ---- etcd: single-node raft. The coreos image is FROM scratch (no /bin/sh), so the exec path
        // is impossible — drive the entrypoint binary directly with run-form `--version`. -------------
        scen("databases/etcd-version-35", "quay.io/coreos/etcd:v3.5.13")
            .run(&["etcd", "--version"])
            .has("etcd Version: 3.5"),

        // ---- memcached: text protocol over nc (alpine busybox), glibc banner -------------------------
        scen("databases/memcached-roundtrip", "memcached:1.6-alpine")
            .exec("memcached -d -u root -l 127.0.0.1 -p 11211\nsleep 1\n( printf 'set dd 0 0 5\\r\\nhello\\r\\nget dd\\r\\nquit\\r\\n'; sleep 0.3 ) | nc 127.0.0.1 11211")
            .has("VALUE dd 0 5").has("hello").timeout(60),
        scen("databases/memcached-version", "memcached:1.6")
            .exec("memcached --version | grep -o '1\\.6'")
            .has("1.6").timeout(60).long(),

        // ---- couchdb: erlang/BEAM, HTTP over loopback via curl ---------------------------------------
        scen("databases/couchdb-welcome", "couchdb:3")
            .exec("export COUCHDB_USER=admin COUCHDB_PASSWORD=pw\n/docker-entrypoint.sh /opt/couchdb/bin/couchdb >/tmp/couch.log 2>&1 &\nfor i in $(seq 1 60); do curl -s http://admin:pw@127.0.0.1:5984/ 2>/dev/null | grep -q Welcome && break; sleep 1; done\ncurl -s http://admin:pw@127.0.0.1:5984/")
            .has("\"couchdb\":\"Welcome\"").timeout(120).long(),
        scen("databases/couchdb-create-db", "couchdb:3")
            .exec("export COUCHDB_USER=admin COUCHDB_PASSWORD=pw\n/docker-entrypoint.sh /opt/couchdb/bin/couchdb >/tmp/couch.log 2>&1 &\nfor i in $(seq 1 60); do curl -s http://admin:pw@127.0.0.1:5984/ 2>/dev/null | grep -q Welcome && break; sleep 1; done\ncurl -s -X PUT http://admin:pw@127.0.0.1:5984/ddb")
            .has("\"ok\":true").timeout(120).long(),

        // ---- influxdb: Go runtime, version banner (deterministic, no server needed) ------------------
        scen("databases/influxdb-version-27", "influxdb:2.7")
            .exec("influxd version 2>&1 | grep -o 'InfluxDB v2.7'")
            .has("InfluxDB v2.7").timeout(60).long(),
        scen("databases/influxdb-version-18", "influxdb:1.8")
            .exec("influxd version 2>&1 | grep -o '1\\.8'")
            .has("1.8").timeout(60).long(),
    ])
}

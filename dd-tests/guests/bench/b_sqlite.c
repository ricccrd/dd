// Real software: SQLite (compiled from the upstream amalgamation, so it builds for both arches).
// An in-memory DB: insert 200k rows in one transaction, index, then aggregate + ordered queries.
// Exercises the full SQLite VDBE, B-tree, sorter and malloc churn — a heavy real-world JIT workload.
#include <stdio.h>
#include <stdlib.h>
#include "sqlite3.h"
static void run(sqlite3 *db, const char *sql) {
    char *err = 0;
    if (sqlite3_exec(db, sql, 0, 0, &err) != SQLITE_OK) { fprintf(stderr, "sql: %s\n", err); exit(2); }
}
int main(void) {
    sqlite3 *db;
    if (sqlite3_open(":memory:", &db) != SQLITE_OK) return 2;
    run(db, "PRAGMA journal_mode=MEMORY; CREATE TABLE t(id INTEGER PRIMARY KEY, k INTEGER, v REAL, s TEXT);");
    run(db, "BEGIN;");
    sqlite3_stmt *st;
    sqlite3_prepare_v2(db, "INSERT INTO t(k,v,s) VALUES(?,?,?)", -1, &st, 0);
    unsigned seed = 12345;
    for (int i = 0; i < 600000; i++) {
        seed = seed * 1103515245u + 12345u;
        sqlite3_bind_int(st, 1, seed % 100000);
        sqlite3_bind_double(st, 2, (seed >> 8) * 1.5);
        char b[24]; snprintf(b, sizeof b, "row-%u", seed % 9973);
        sqlite3_bind_text(st, 3, b, -1, SQLITE_TRANSIENT);
        sqlite3_step(st); sqlite3_reset(st);
    }
    sqlite3_finalize(st);
    run(db, "COMMIT;");
    run(db, "CREATE INDEX ik ON t(k);");
    long checksum = 0;
    sqlite3_prepare_v2(db, "SELECT k, COUNT(*), SUM(v) FROM t GROUP BY k ORDER BY 2 DESC LIMIT 50", -1, &st, 0);
    while (sqlite3_step(st) == SQLITE_ROW) checksum += sqlite3_column_int(st, 0) + sqlite3_column_int(st, 1);
    sqlite3_finalize(st);
    sqlite3_close(db);
    printf("%ld\n", checksum);
    return 0;
}

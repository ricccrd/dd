// A postgres-shaped workload in miniature: a TCP server on 127.0.0.1 backed by a real
// SQLite database, driven by a forked client over many short-lived connections. This is the
// "long-running networked service" acid test -- it combines an accept loop, per-connection
// socket I/O, fork, and a real storage engine (WAL, transactions) all at once. The client
// opens 50 connections issuing INSERTs, then one final connection asks for the row COUNT and
// the SUM; the parent prints the server's answer. aarch64-only (links libsqlite3, like the
// sqlite case) and diffed against a native run -> oracle.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#define N 50

static int read_line(int fd, char *buf, int max) {
    int i = 0;
    while (i < max - 1) {
        char ch;
        ssize_t r = read(fd, &ch, 1);
        if (r <= 0) break;
        if (ch == '\n') break;
        buf[i++] = ch;
    }
    buf[i] = 0;
    return i;
}

int main(void) {
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    bind(ls, (struct sockaddr *)&a, sizeof a);
    listen(ls, 16);
    socklen_t al = sizeof a;
    getsockname(ls, (struct sockaddr *)&a, &al);
    int port = ntohs(a.sin_port);

    pid_t pid = fork();
    if (pid == 0) { // ---- client ----
        close(ls);
        for (int i = 0; i < N; i++) {
            int cs = socket(AF_INET, SOCK_STREAM, 0);
            struct sockaddr_in s = {0};
            s.sin_family = AF_INET;
            s.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            s.sin_port = htons(port);
            connect(cs, (struct sockaddr *)&s, sizeof s);
            char cmd[32];
            int m = snprintf(cmd, sizeof cmd, "PUT %d\n", i + 1);
            write(cs, cmd, m);
            char rb[32];
            read_line(cs, rb, sizeof rb); // "ok"
            close(cs);
        }
        // final query connection
        int cs = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in s = {0};
        s.sin_family = AF_INET;
        s.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        s.sin_port = htons(port);
        connect(cs, (struct sockaddr *)&s, sizeof s);
        write(cs, "STAT\n", 5);
        char rb[64] = {0};
        read_line(cs, rb, sizeof rb);
        printf("dbserver %s\n", rb);
        fflush(stdout); // _exit() skips the stdio flush
        close(cs);
        _exit(0);
    }

    // ---- server ----
    sqlite3 *db;
    sqlite3_open("/tmp/dd_dbserver.db", &db);
    sqlite3_exec(db, "PRAGMA journal_mode=WAL; DROP TABLE IF EXISTS t; CREATE TABLE t(v INTEGER);",
                 0, 0, 0);
    for (;;) {
        int cs = accept(ls, NULL, NULL);
        if (cs < 0) break;
        char line[64];
        read_line(cs, line, sizeof line);
        if (strncmp(line, "PUT ", 4) == 0) {
            char sql[96];
            snprintf(sql, sizeof sql, "INSERT INTO t(v) VALUES(%d);", atoi(line + 4));
            sqlite3_exec(db, sql, 0, 0, 0);
            write(cs, "ok\n", 3);
            close(cs);
        } else if (strncmp(line, "STAT", 4) == 0) {
            sqlite3_stmt *st;
            sqlite3_prepare_v2(db, "SELECT COUNT(*), COALESCE(SUM(v),0) FROM t;", -1, &st, 0);
            sqlite3_step(st);
            char out[64];
            int m = snprintf(out, sizeof out, "count=%d sum=%d\n",
                             sqlite3_column_int(st, 0), sqlite3_column_int(st, 1));
            sqlite3_finalize(st);
            write(cs, out, m);
            close(cs);
            break;
        } else {
            close(cs);
        }
    }
    sqlite3_close(db);
    waitpid(pid, NULL, 0);
    unlink("/tmp/dd_dbserver.db");
    return 0;
}

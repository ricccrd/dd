// Real SQLite (the actual engine, static-linked) doing real DB work — a realistic container workload:
// exercises file I/O, mmap, fsync, pread/pwrite, locking. usage: sqlite_app <db-path>
#include <stdio.h>
#include <sqlite3.h>
int main(int c, char**v){
  const char* path = c>1 ? v[1] : "test.db";
  sqlite3* db; if(sqlite3_open(path,&db)){ printf("open fail: %s\n", sqlite3_errmsg(db)); return 1; }
  char* err=0;
  sqlite3_exec(db,"PRAGMA journal_mode=WAL; CREATE TABLE IF NOT EXISTS t(id INTEGER PRIMARY KEY, v INTEGER, s TEXT);",0,0,&err);
  sqlite3_exec(db,"DELETE FROM t;",0,0,&err);
  sqlite3_exec(db,"BEGIN;",0,0,&err);
  sqlite3_stmt* st; sqlite3_prepare_v2(db,"INSERT INTO t(v,s) VALUES(?,?)",-1,&st,0);
  for(int i=1;i<=5000;i++){ char s[32]; snprintf(s,sizeof s,"row-%d",i*7%100);
    sqlite3_bind_int(st,1,i*i%1000003); sqlite3_bind_text(st,2,s,-1,SQLITE_TRANSIENT);
    sqlite3_step(st); sqlite3_reset(st); }
  sqlite3_finalize(st); sqlite3_exec(db,"COMMIT;",0,0,&err);
  sqlite3_stmt* q; sqlite3_prepare_v2(db,"SELECT COUNT(*), SUM(v), MIN(s), MAX(s) FROM t",-1,&q,0);
  sqlite3_step(q);
  printf("SQLITE rows=%d sum=%lld min=%s max=%s\n", sqlite3_column_int(q,0),
         (long long)sqlite3_column_int64(q,1), sqlite3_column_text(q,2), sqlite3_column_text(q,3));
  sqlite3_finalize(q); sqlite3_close(db); return 0;
}

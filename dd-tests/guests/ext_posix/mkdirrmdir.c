// mkdir/rmdir lifecycle: create, EEXIST on repeat, rmdir, then ENOENT.
#include <errno.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

int main(void) {
    char dir[128];
    snprintf(dir, sizeof dir, "/tmp/dd_mkdir_%d", (int)getpid());
    rmdir(dir);
    int made = mkdir(dir, 0755) == 0;
    struct stat st;
    int isdir = stat(dir, &st) == 0 && S_ISDIR(st.st_mode);
    int again = mkdir(dir, 0755) < 0 && errno == EEXIST;
    int removed = rmdir(dir) == 0;
    int gone = stat(dir, &st) < 0 && errno == ENOENT;
    printf("mkdirrmdir made=%d isdir=%d eexist=%d removed=%d gone=%d\n", made, isdir, again, removed, gone);
    return 0;
}

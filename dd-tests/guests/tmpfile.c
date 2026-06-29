// Temp-file creation: mkstemp() mints a unique file, we write+rewind+read it, then stdio tmpfile()
// does the same via a FILE*. Prints a verdict (paths are random). Exercises mkstemp/unlink and the
// tmpfile() auto-cleanup path. Portable -> all engines, golden-checked.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void) {
    char tmpl[] = "/tmp/dd_mkstempXXXXXX";
    int fd = mkstemp(tmpl);
    int mk_ok = fd >= 0;
    char rb[32] = {0};
    if (mk_ok) {
        write(fd, "abc123", 6);
        lseek(fd, 0, SEEK_SET);
        read(fd, rb, sizeof rb - 1);
        close(fd);
        unlink(tmpl);
    }
    int mk_data = strcmp(rb, "abc123") == 0;

    FILE *tf = tmpfile();
    int tf_ok = tf != NULL;
    long val = -1;
    if (tf_ok) {
        fprintf(tf, "%d", 4242);
        rewind(tf);
        fscanf(tf, "%ld", &val);
        fclose(tf);
    }
    printf("tmpfile mkstemp=%d data=%d tmpfile=%d val=%ld\n", mk_ok, mk_data, tf_ok, val);
    return 0;
}

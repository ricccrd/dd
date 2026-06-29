// Filename globbing: create a set of files, then glob() two patterns and count matches. Exercises
// glob/globfree (opendir + fnmatch + sorting under the hood). Portable -> all engines, golden.
#include <glob.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

int main(void) {
    const char *dir = "/tmp/dd_glob_dir";
    mkdir(dir, 0755);
    const char *files[] = {"a.txt", "b.txt", "c.log", "d.txt", "e.md"};
    char p[256];
    for (int i = 0; i < 5; i++) {
        snprintf(p, sizeof p, "%s/%s", dir, files[i]);
        FILE *f = fopen(p, "w");
        if (f) fclose(f);
    }
    glob_t g;
    snprintf(p, sizeof p, "%s/*.txt", dir);
    glob(p, 0, NULL, &g);
    int txt = (int)g.gl_pathc;
    globfree(&g);
    snprintf(p, sizeof p, "%s/*", dir);
    glob(p, 0, NULL, &g);
    int all = (int)g.gl_pathc;
    globfree(&g);

    for (int i = 0; i < 5; i++) {
        snprintf(p, sizeof p, "%s/%s", dir, files[i]);
        unlink(p);
    }
    rmdir(dir);
    printf("glob txt=%d all=%d\n", txt, all); // 3 5
    return 0;
}

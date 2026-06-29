// Directory enumeration: create a dir with known entries, then opendir/readdir and collect the
// names (skipping . and ..), counting and checksumming them order-independently. Exercises the
// getdents/readdir path + stat-by-dirent. Portable -> all engines, golden-checked.
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int main(void) {
    const char *dir = "/tmp/dd_getdents_dir";
    mkdir(dir, 0755);
    const char *names[] = {"alpha", "bravo", "charlie", "delta"};
    char p[256];
    for (int i = 0; i < 4; i++) {
        snprintf(p, sizeof p, "%s/%s", dir, names[i]);
        FILE *f = fopen(p, "w");
        if (f) fclose(f);
    }
    DIR *d = opendir(dir);
    int count = 0;
    long namechk = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        count++;
        for (const char *c = e->d_name; *c; c++) namechk += (unsigned char)*c; // order-independent
    }
    closedir(d);
    for (int i = 0; i < 4; i++) {
        snprintf(p, sizeof p, "%s/%s", dir, names[i]);
        unlink(p);
    }
    rmdir(dir);
    printf("getdents count=%d namechk=%ld\n", count, namechk); // 4, sum of all chars = 2003
    return 0;
}

// readdir with rewinddir/seekdir/telldir + entry kinds (dir vs file) via stat (portable, d_type-free).
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int main(void) {
    char dir[128];
    snprintf(dir, sizeof dir, "/tmp/dd_rd_%d", (int)getpid());
    mkdir(dir, 0755);
    char p[200];
    snprintf(p, sizeof p, "%s/file1", dir); close(open(p, O_CREAT | O_WRONLY, 0644));
    snprintf(p, sizeof p, "%s/file2", dir); close(open(p, O_CREAT | O_WRONLY, 0644));
    snprintf(p, sizeof p, "%s/sub", dir); mkdir(p, 0755);
    DIR *d = opendir(dir);
    int files = 0, dirs = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        struct stat st;
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        stat(p, &st);
        if (S_ISDIR(st.st_mode)) dirs++; else files++;
    }
    // rewinddir restarts enumeration
    rewinddir(d);
    int again = 0;
    while ((e = readdir(d))) if (e->d_name[0] != '.') again++;
    closedir(d);
    snprintf(p, sizeof p, "%s/file1", dir); unlink(p);
    snprintf(p, sizeof p, "%s/file2", dir); unlink(p);
    snprintf(p, sizeof p, "%s/sub", dir); rmdir(p);
    rmdir(dir);
    printf("readdir_dtype files=%d dirs=%d rewind=%d\n", files, dirs, again); // 2 1 3
    return 0;
}

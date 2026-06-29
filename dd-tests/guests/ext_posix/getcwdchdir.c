// getcwd/chdir: create a unique subdir, chdir into it, getcwd must end with that subdir.
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int main(void) {
    char name[64];
    snprintf(name, sizeof name, "dd_cwd_%d", (int)getpid());
    char dir[128];
    snprintf(dir, sizeof dir, "/tmp/%s", name);
    mkdir(dir, 0755);
    char before[256] = {0};
    int got_before = getcwd(before, sizeof before) != NULL && before[0] == '/';
    int ch = chdir(dir) == 0;
    char after[256] = {0};
    getcwd(after, sizeof after);
    size_t la = strlen(after), ln = strlen(name);
    int ends = la >= ln && strcmp(after + la - ln, name) == 0;
    chdir("/");
    rmdir(dir);
    printf("getcwdchdir before=%d chdir=%d ends=%d\n", got_before, ch, ends);
    return 0;
}

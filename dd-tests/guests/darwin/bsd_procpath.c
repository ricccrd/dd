// macOS-native libproc proc_pidpath: get our own executable path from our pid. A darwin/libproc API
// with no Linux equivalent (Linux uses /proc/self/exe). darwin engine only, golden-checked.
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <libproc.h>

int main(void) {
    char buf[4096];
    int n = proc_pidpath(getpid(), buf, sizeof buf);
    int ok = (n > 0) && (strlen(buf) > 0);
    printf("proc_pidpath ok=%d\n", ok); // 1
    return 0;
}

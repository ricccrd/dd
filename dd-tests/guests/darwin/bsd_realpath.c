// macOS-native realpath: "/tmp" canonicalizes (to /private/tmp via the darwin firmlink). Exercises
// the BSD path-resolution + symlink behavior. darwin engine only, golden-checked.
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

int main(void) {
    char buf[1024];
    char *r = realpath("/tmp", buf);
    int ok = (r != NULL) && (strstr(buf, "tmp") != NULL);
    printf("realpath ok=%d\n", ok); // 1
    return 0;
}

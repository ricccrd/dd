// macOS-native getattrlist(ATTR_CMN_NAME): read a file's name attribute via the BSD bulk-attribute
// interface. No Linux equivalent. darwin engine only, golden-checked.
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/attr.h>

struct nameattr {
    uint32_t length;
    attrreference_t ref;
    char name[256];
};

int main(void) {
    char path[] = "/tmp/ddattrXXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) { printf("getattrlist ok=0\n"); return 0; }
    write(fd, "hello", 5);
    close(fd);
    struct attrlist al = {0};
    al.bitmapcount = ATTR_BIT_MAP_COUNT;
    al.commonattr = ATTR_CMN_NAME;
    struct nameattr buf;
    int r = getattrlist(path, &al, &buf, sizeof buf, 0);
    char *name = ((char *)&buf.ref) + buf.ref.attr_dataoffset;
    char *base = strrchr(path, '/') + 1;
    int ok = (r == 0) && (strcmp(name, base) == 0);
    unlink(path);
    printf("getattrlist ok=%d\n", ok); // 1
    return 0;
}

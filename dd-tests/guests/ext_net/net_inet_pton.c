// inet_pton/inet_ntop round-trips for IPv4 and IPv6, plus rejection of a malformed IPv4 literal
// (returns 0). Verifies textual<->binary address conversion. Portable -> all engines, golden.
#include <arpa/inet.h>
#include <stdio.h>
int main(void) {
    struct in_addr v4; inet_pton(AF_INET, "192.168.1.42", &v4);
    char b4[INET_ADDRSTRLEN]; inet_ntop(AF_INET, &v4, b4, sizeof b4);
    struct in6_addr v6; inet_pton(AF_INET6, "2001:db8::1", &v6);
    char b6[INET6_ADDRSTRLEN]; inet_ntop(AF_INET6, &v6, b6, sizeof b6);
    int bad = inet_pton(AF_INET, "999.1.1.1", &v4);
    printf("inet_pton v4=%s v6=%s bad=%d\n", b4, b6, bad); // 192.168.1.42 2001:db8::1 0
    return 0;
}

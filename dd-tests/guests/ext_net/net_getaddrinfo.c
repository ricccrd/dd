// getaddrinfo in numeric mode (AI_NUMERICHOST|AI_NUMERICSERV): resolve 127.0.0.1:8080 with no DNS,
// confirm the parsed address+port via inet_ntop. Portable -> all engines, golden.
#include <netdb.h>
#include <stdio.h>
#include <arpa/inet.h>
#include <netinet/in.h>
int main(void) {
    struct addrinfo hints = {0}, *res;
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM; hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV;
    int r = getaddrinfo("127.0.0.1", "8080", &hints, &res);
    int ok = 0; char ipstr[64] = "";
    if (r == 0) { struct sockaddr_in *s = (struct sockaddr_in *)res->ai_addr; inet_ntop(AF_INET, &s->sin_addr, ipstr, sizeof ipstr); ok = (ntohs(s->sin_port) == 8080); freeaddrinfo(res); }
    printf("getaddrinfo r=%d ip=%s port_ok=%d\n", r, ipstr, ok); // 0 127.0.0.1 1
    return 0;
}

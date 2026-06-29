// SO_LINGER / SO_KEEPALIVE / SO_TYPE: set linger {on,5} and keepalive, read them back, and confirm
// SO_TYPE reports SOCK_STREAM. Verifies struct-valued and flag socket options. Portable, golden.
#include <sys/socket.h>
#include <stdio.h>
#include <unistd.h>
int main(void) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct linger lg = {1, 5}; setsockopt(s, SOL_SOCKET, SO_LINGER, &lg, sizeof lg);
    struct linger got = {0, 0}; socklen_t l = sizeof got; getsockopt(s, SOL_SOCKET, SO_LINGER, &got, &l);
    int ka = 1; setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, &ka, sizeof ka);
    int kag = 0; l = sizeof kag; getsockopt(s, SOL_SOCKET, SO_KEEPALIVE, &kag, &l);
    int type = 0; l = sizeof type; getsockopt(s, SOL_SOCKET, SO_TYPE, &type, &l);
    close(s);
    printf("so_linger on=%d t=%d keepalive=%d type_stream=%d\n", got.l_onoff != 0, got.l_linger, kag != 0, type == SOCK_STREAM);
    return 0;
}

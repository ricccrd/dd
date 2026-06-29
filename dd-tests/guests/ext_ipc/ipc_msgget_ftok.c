// ftok-derived key + msgget: build a key from an existing path, create a queue, round-trip one
// message. Verifies ftok() and key-based (non-IPC_PRIVATE) queue lookup. Portable -> all, golden.
#include <stdio.h>
#include <sys/ipc.h>
#include <sys/msg.h>
struct m { long type; char t[16]; };
int main(void) {
    key_t k = ftok("/tmp", 42);
    if (k == -1) { perror("ftok"); return 1; }
    int id = msgget(k, IPC_CREAT | 0600);
    if (id < 0) { perror("msgget"); return 1; }
    struct m s = {1, "ftok-msg"}; msgsnd(id, &s, sizeof s.t, 0);
    struct m r; msgrcv(id, &r, sizeof r.t, 1, 0);
    msgctl(id, IPC_RMID, 0);
    printf("ftok key_ok=%d msg=%s\n", k != -1, r.t); // 1 ftok-msg
    return 0;
}

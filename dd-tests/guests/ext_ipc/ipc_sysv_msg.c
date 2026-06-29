// System V message queue typed receive: send 3 messages of types 1/2/3, then receive by exact type
// (2), by "any" (lowest, =1), and by type 3. Verifies msgsnd/msgrcv type selection. Portable, golden.
#include <stdio.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/msg.h>
struct mq_item { long type; char text[32]; };
int main(void) {
    int id = msgget(IPC_PRIVATE, IPC_CREAT | 0600);
    if (id < 0) { perror("msgget"); return 1; }
    struct mq_item m;
    for (long t = 1; t <= 3; t++) { m.type = t; snprintf(m.text, 32, "type%ld", t); msgsnd(id, &m, sizeof m.text, 0); }
    struct mq_item r;
    msgrcv(id, &r, sizeof r.text, 2, 0); char a[32]; strcpy(a, r.text);
    msgrcv(id, &r, sizeof r.text, 0, 0); char b[32]; strcpy(b, r.text);
    msgrcv(id, &r, sizeof r.text, 3, 0); char c[32]; strcpy(c, r.text);
    msgctl(id, IPC_RMID, 0);
    printf("sysv_msg t2=%s any=%s t3=%s\n", a, b, c); // type2 type1 type3
    return 0;
}

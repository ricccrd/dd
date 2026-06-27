// SysV message queue: send a message, receive it back. Exercises msgget/msgsnd/msgrcv/msgctl(IPC_RMID).
#include <stdio.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/msg.h>

struct msg { long mtype; char mtext[32]; };

int main(void) {
    int id = msgget(IPC_PRIVATE, IPC_CREAT | 0666);
    if (id < 0) { perror("msgget"); return 1; }
    struct msg s = {1, "MSG-PAYLOAD"};
    if (msgsnd(id, &s, sizeof s.mtext, 0) < 0) { perror("msgsnd"); return 1; }
    struct msg r;
    memset(&r, 0, sizeof r);
    if (msgrcv(id, &r, sizeof r.mtext, 1, 0) < 0) { perror("msgrcv"); return 1; }
    printf("MSG=%s\n", r.mtext);         // expect MSG-PAYLOAD
    msgctl(id, IPC_RMID, NULL);
    return 0;
}

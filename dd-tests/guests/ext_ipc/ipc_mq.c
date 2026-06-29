// POSIX message queue with priorities: send "low"/"high"/"mid" at priorities 1/9/5; mq_receive must
// return them highest-priority-first. (Linux-only; macOS has no POSIX mq.) Diffed vs native oracle.
#include <mqueue.h>
#include <stdio.h>
#include <fcntl.h>
int main(void) {
    const char *name = "/dd_mq_test";
    mq_unlink(name);
    struct mq_attr at = {0}; at.mq_maxmsg = 8; at.mq_msgsize = 32;
    mqd_t q = mq_open(name, O_CREAT | O_RDWR, 0600, &at);
    if (q == (mqd_t)-1) { perror("mq_open"); return 1; }
    mq_send(q, "low", 3, 1);
    mq_send(q, "high", 4, 9);
    mq_send(q, "mid", 3, 5);
    char buf[64]; unsigned prio;
    ssize_t n1 = mq_receive(q, buf, 64, &prio); buf[n1 > 0 ? n1 : 0] = 0; printf("1=%s/%u\n", buf, prio);
    ssize_t n2 = mq_receive(q, buf, 64, &prio); buf[n2 > 0 ? n2 : 0] = 0; printf("2=%s/%u\n", buf, prio);
    ssize_t n3 = mq_receive(q, buf, 64, &prio); buf[n3 > 0 ? n3 : 0] = 0; printf("3=%s/%u\n", buf, prio);
    mq_close(q); mq_unlink(name);
    return 0; // 1=high/9 2=mid/5 3=low/1
}

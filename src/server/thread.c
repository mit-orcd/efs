#include "server_internal.h"

/* Conn/writer stacks: hello_ack is heap-allocated now, so 1 MiB is ample and
 * avoids ~8 GiB of VA when 512 conn threads are live. */
#define EFSD_THREAD_STACK (1 * 1024 * 1024)

int efsd_pthread_create(pthread_t *tid, void *(*fn)(void *), void *arg)
{
    pthread_attr_t attr;
    int rc;
    if (pthread_attr_init(&attr) != 0)
        return pthread_create(tid, NULL, fn, arg);
    if (pthread_attr_setstacksize(&attr, EFSD_THREAD_STACK) != 0) {
        pthread_attr_destroy(&attr);
        return pthread_create(tid, NULL, fn, arg);
    }
    rc = pthread_create(tid, &attr, fn, arg);
    pthread_attr_destroy(&attr);
    return rc;
}


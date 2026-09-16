#include "efs/dir_spread.h"
#include <pthread.h>
#include <string.h>

#define SPREAD_QMAX 256

static efs_ino_t q[SPREAD_QMAX];
static uint32_t qn;
static pthread_mutex_t qmu = PTHREAD_MUTEX_INITIALIZER;

void efs_dir_spread_note(efs_ino_t ino)
{
    uint32_t i;

    if (ino == 0)
        return;
    pthread_mutex_lock(&qmu);
    for (i = 0; i < qn; i++) {
        if (q[i] == ino) {
            pthread_mutex_unlock(&qmu);
            return;
        }
    }
    if (qn < SPREAD_QMAX)
        q[qn++] = ino;
    pthread_mutex_unlock(&qmu);
}

int efs_dir_spread_pop(efs_ino_t *ino)
{
    if (!ino)
        return 0;
    pthread_mutex_lock(&qmu);
    if (qn == 0) {
        pthread_mutex_unlock(&qmu);
        return 0;
    }
    *ino = q[0];
    if (qn > 1)
        memmove(q, q + 1, (qn - 1) * sizeof(q[0]));
    qn--;
    pthread_mutex_unlock(&qmu);
    return 1;
}

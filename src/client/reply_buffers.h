#ifndef EFS_REPLY_BUFFERS_H
#define EFS_REPLY_BUFFERS_H

#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* The pthread key owns both reusable buffers, including failed-growth
 * leftovers. Compiler TLS alone does not free heap storage at worker exit. */
struct reply_buffers {
    char *data[2];
    size_t capacity[2];
};
static pthread_key_t reply_buffers_key;
static pthread_once_t reply_buffers_once = PTHREAD_ONCE_INIT;
static int reply_buffers_key_error;
static int reply_buffers_trace;

#if defined(EFS_FAULTS) && EFS_FAULTS
/* Test-only failures at this owner boundary, not arbitrary libc allocation.
 * Every seventeenth matching attempt fails so real workers can recover. */
static int reply_buffers_fault_kind;
static unsigned long reply_buffers_fault_count;
static int reply_buffers_fault(int kind)
{
    if (kind != reply_buffers_fault_kind ||
        (__sync_add_and_fetch(&reply_buffers_fault_count, 1) % 17) != 0)
        return 0;
    fprintf(stderr,"reply-buffer fault kind=%d pid=%ld\n",kind,(long)getpid());
    return 1;
}
#else
#define reply_buffers_fault(kind) 0
#endif

static void reply_buffers_event(const char *event, const struct reply_buffers *b)
{
    if (reply_buffers_trace)
        fprintf(stderr, "reply-buffer %s owner=%p read=%zu readdir=%zu pid=%ld\n",
                event, (const void *)b, b->capacity[0], b->capacity[1], (long)getpid());
}


static void reply_buffers_destroy(void *value)
{
    struct reply_buffers *b = value;
    if (b) {
        reply_buffers_event("retire", b);
        free(b->data[0]);
        free(b->data[1]);
        free(b);
    }
}

static void reply_buffers_init(void)
{
    const char *trace = getenv("EFS_REPLY_BUFFER_TRACE");
    reply_buffers_trace = trace && trace[0] && strcmp(trace,"0") != 0;
#if defined(EFS_FAULTS) && EFS_FAULTS
    const char *fault = getenv("EFS_FAULT_REPLY_BUFFER");
    if (fault) {
        if (!strcmp(fault,"allocation")) reply_buffers_fault_kind=1;
        else if (!strcmp(fault,"tls")) reply_buffers_fault_kind=2;
        else if (!strcmp(fault,"growth")) reply_buffers_fault_kind=3;
    }
#endif
    reply_buffers_key_error =
        pthread_key_create(&reply_buffers_key, reply_buffers_destroy);
}

static char *reply_buffer_get(unsigned which, size_t size)
{
    struct reply_buffers *b;
    char *grown;
    if (which >= 2 || pthread_once(&reply_buffers_once, reply_buffers_init) ||
        reply_buffers_key_error)
        return NULL;
    b = pthread_getspecific(reply_buffers_key);
    if (!b) {
        if (reply_buffers_fault(1)) return NULL;
        b = calloc(1, sizeof(*b));
        if (!b)
            return NULL;
        if (reply_buffers_fault(2) || pthread_setspecific(reply_buffers_key, b)) {
            free(b);
            return NULL;
        }
        reply_buffers_event("new", b);
    }
    if (!size)
        size = 1;
    if (b->capacity[which] < size) {
        if (reply_buffers_fault(3)) return NULL;
        grown = realloc(b->data[which], size);
        if (!grown)
            return NULL;
        b->data[which] = grown;
        b->capacity[which] = size;
        reply_buffers_event("grow", b);
    }
    return b->data[which];
}

#endif

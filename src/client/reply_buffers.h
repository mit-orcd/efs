#ifndef EFS_REPLY_BUFFERS_H
#define EFS_REPLY_BUFFERS_H

#include <pthread.h>
#include <stdlib.h>

/* The pthread key owns both reusable buffers, including failed-growth
 * leftovers. Compiler TLS alone does not free heap storage at worker exit. */
struct reply_buffers {
    char *data[2];
    size_t capacity[2];
};
static pthread_key_t reply_buffers_key;
static pthread_once_t reply_buffers_once = PTHREAD_ONCE_INIT;
static int reply_buffers_key_error;

static void reply_buffers_destroy(void *value)
{
    struct reply_buffers *b = value;
    if (b) {
        free(b->data[0]);
        free(b->data[1]);
        free(b);
    }
}

static void reply_buffers_init(void)
{
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
        b = calloc(1, sizeof(*b));
        if (!b)
            return NULL;
        if (pthread_setspecific(reply_buffers_key, b)) {
            free(b);
            return NULL;
        }
    }
    if (!size)
        size = 1;
    if (b->capacity[which] < size) {
        grown = realloc(b->data[which], size);
        if (!grown)
            return NULL;
        b->data[which] = grown;
        b->capacity[which] = size;
    }
    return b->data[which];
}

#endif

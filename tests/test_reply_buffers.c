/* Standalone pthread lifetime and failure tests for the production helper. */
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>
static atomic_size_t outstanding;
static __thread int fail_alloc, fail_register;
static int fail_key;
static void *tracked_malloc(size_t n)
{
    if (fail_alloc) return NULL;
    size_t *p = malloc(sizeof(*p) + n);
    if (!p) return NULL;
    *p = n;
    atomic_fetch_add(&outstanding, n);
    return p + 1;
}
static void tracked_free(void *p)
{
    if (!p) return;
    size_t *q = (size_t *)p - 1;
    atomic_fetch_sub(&outstanding, *q);
    free(q);
}
static void *tracked_calloc(size_t n, size_t size)
{
    void *p = tracked_malloc(n * size);
    if (p) memset(p, 0, n * size);
    return p;
}
static void *tracked_realloc(void *p, size_t n)
{
    void *q = tracked_malloc(n);
    if (!q) return NULL;
    if (p) {
        size_t old = *((size_t *)p - 1);
        memcpy(q, p, old < n ? old : n);
        tracked_free(p);
    }
    return q;
}
static int test_setspecific(pthread_key_t key, const void *p)
{ return fail_register ? 1 : pthread_setspecific(key, p); }
static int test_key_create(pthread_key_t *key, void (*destroy)(void *))
{ return fail_key ? 1 : pthread_key_create(key, destroy); }
#define calloc tracked_calloc
#define realloc tracked_realloc
#define free tracked_free
#define pthread_setspecific test_setspecific
#define pthread_key_create test_key_create
#include "../src/client/reply_buffers.h"
#undef calloc
#undef realloc
#undef free
#undef pthread_setspecific
#undef pthread_key_create
static void *worker(void *arg)
{
    (void)arg;
    fail_alloc = 1;
    assert(!reply_buffer_get(0, 16));
    fail_alloc = 0;
    fail_register = 1;
    assert(!reply_buffer_get(0, 16));
    fail_register = 0;
    char *r = reply_buffer_get(0, 4096);
    char *d = reply_buffer_get(1, 8192);
    assert(r && d && r != d);
    assert(reply_buffer_get(0, 1024) == r);
    memset(r, 42, 4096);
    fail_alloc = 1;
    assert(!reply_buffer_get(0, 1u << 20));
    assert(reply_buffer_get(0, 4096) == r && r[0] == 42);
    fail_alloc = 0;
    r = reply_buffer_get(0, 1u << 20);
    assert(r && r[0] == 42);
    assert(reply_buffer_get(1, 8192) == d);
    return NULL;
}
int main(int argc, char **argv)
{
    (void)argv;
    if (argc > 1) {
        fail_key = 1;
        assert(!reply_buffer_get(0, 4096));
        assert(!atomic_load(&outstanding));
        puts("test_reply_buffers: key failure OK");
        return 0;
    }
    for (int round = 0; round < 4; round++) {
        pthread_t threads[32];
        for (int i = 0; i < 32; i++) assert(!pthread_create(&threads[i], NULL, worker, NULL));
        for (int i = 0; i < 32; i++) assert(!pthread_join(threads[i], NULL));
        assert(!atomic_load(&outstanding));
    }
    puts("test_reply_buffers: OK (128 retired workers, reuse, growth, allocation/TLS failures)");
    return 0;
}

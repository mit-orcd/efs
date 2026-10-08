#ifndef EFS_PRINCIPLES_INDEX_H
#define EFS_PRINCIPLES_INDEX_H
#include <stdint.h>
/* Independent experimental protocol. Caller serializes one handle; the private
 * root is exclusively locked. No production store, GC or session calls. */
struct principles_index;
struct principles_change {
    uint32_t object, len;
    uint64_t expected_generation;
    const void *data; /* NULL + len=0 means conditional deletion */
};
struct principles_index_stats {
    uint64_t live_bytes, segment_bytes, index_bytes, allocated_bytes, garbage_bytes, syncs,
        body_written_bytes, index_written_bytes;
};
enum principles_index_stage {
    PI_BODY_WRITE = 1,
    PI_BODY_SYNC,
    PI_BODY_DIR,
    PI_INDEX_WRITE,
    PI_INDEX_SYNC,
    PI_RENAME,
    PI_PUBLICATION,
    PI_GC
};
/* Hook is for independent crash experiments; NULL in normal measurements. */
typedef void (*principles_index_hook)(void *, enum principles_index_stage);
struct principles_index *principles_index_open(const char *root, uint32_t capacity, uint64_t quota,
                                               int create, int readonly);
void principles_index_set_hook(struct principles_index *, principles_index_hook, void *);
int principles_index_publish(struct principles_index *, const struct principles_change *, uint32_t);
int principles_index_get(struct principles_index *, uint32_t, void *, uint32_t *, uint64_t *);
int principles_index_collect(struct principles_index *, uint64_t *);
int principles_index_stats(struct principles_index *, struct principles_index_stats *);
void principles_index_close(struct principles_index *);
#endif

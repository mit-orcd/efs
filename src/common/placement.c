#include "efs/placement.h"
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <netdb.h>

void efs_get_placement(uint32_t node_count, efs_ino_t ino, uint32_t chunk_index,
                       efs_node_id_t fragment_nodes[EFS_NUM_FRAGMENTS])
{
    if (node_count == 0) node_count = 1;

    uint64_t key = ((uint64_t)ino << 32) | (uint64_t)chunk_index;
    uint64_t hash = 14695981039346656037ULL;
    for (int i = 0; i < 8; i++) {
        hash ^= (key >> (i * 8)) & 0xFFULL;
        hash *= 1099511628211ULL;
    }

    uint32_t start = (uint32_t)(hash % (uint64_t)node_count);
    for (int i = 0; i < EFS_NUM_FRAGMENTS; i++) {
        fragment_nodes[i] = (efs_node_id_t)((start + i) % node_count + 1);
    }
}

static bool host_match(const char *a, const char *b)
{
    size_t i = 0;
    while (a[i] && b[i]) {
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i]))
            return false;
        i++;
    }
    return a[i] == '\0' && b[i] == '\0';
}

bool efs_is_local_node(const struct efs_node *nodes, uint32_t node_count,
                       const char *host, efs_node_id_t *local_id)
{
    char hostname[256];
    if (gethostname(hostname, sizeof(hostname)) != 0)
        hostname[0] = '\0';
    hostname[sizeof(hostname) - 1] = '\0';

    for (uint32_t i = 0; i < node_count; i++) {
        if (host_match(nodes[i].addr, host) ||
            host_match(nodes[i].addr, hostname) ||
            host_match(nodes[i].addr, "localhost") ||
            host_match(nodes[i].addr, "127.0.0.1")) {
            if (local_id)
                *local_id = nodes[i].id;
            return true;
        }
    }
    (void)hostname;
    return false;
}

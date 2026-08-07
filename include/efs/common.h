#ifndef EFS_COMMON_H
#define EFS_COMMON_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <sys/stat.h>

#define EFS_VERSION_MAJOR 0
#define EFS_VERSION_MINOR 1
#define EFS_VERSION_PATCH 0

#define EFS_CHUNK_SIZE       (128 * 1024)
#define EFS_FRAGMENT_SIZE    (EFS_CHUNK_SIZE / 2)
#define EFS_NUM_FRAGMENTS    3
#define EFS_MAX_NODES        4
/* Max TCP connections the client keeps open to each server (pool size).
 * Sized for many FUSE writers × fragment fanout × chunk pipeline. */
#define EFS_CLIENT_CONNS_PER_NODE 32
/* How many 128 KiB chunks one FUSE write may PUT concurrently. */
#define EFS_WRITE_PIPELINE 8
#define EFS_MAX_EXPORTS      16
#define EFS_MAX_PATH         4096
#define EFS_MAX_NAME         256
/* Local data roots per efsd: 1 (plain) or 3..8 (local EC). 2 is rejected. */
#define EFS_MAX_STORAGE_PATHS 8
#define EFS_HASH_SIZE        32
#define EFS_LISTEN_BACKLOG   512
#define EFS_DEFAULT_PORT     7432
#define EFS_HEARTBEAT_MS     2000
#define EFS_IO_TIMEOUT_MS    30000

#define EFS_ROOT_INO         1
/* Reserved inode for 2+1 metadata table pages (not a user-visible file).
 * High bit set so it cannot collide with client inode namespaces. */
#define EFS_META_TABLE_INO   ((efs_ino_t)0x8000000000000002ULL)
/* Max 128 KiB pages in a fragmented metadata blob (~64 MiB logical). */
#define EFS_META_MAX_PAGES   512
/* On-disk inode directory sharding: five base-10000 groups (0000-9999)
 * encode any uint64 ino uniquely (10^20 > 2^64). seg[0] is least-significant. */
#define EFS_INO_PATH_SEGS    5

#define EFS_OK               0
#define EFS_ERR_IO          -1
#define EFS_ERR_NOMEM       -2
#define EFS_ERR_NOT_FOUND   -3
#define EFS_ERR_EXIST       -4
#define EFS_ERR_INVAL       -5
#define EFS_ERR_NET         -6
#define EFS_ERR_PROTO       -7
#define EFS_ERR_NO_QUORUM   -8
#define EFS_ERR_DECODE      -9
#define EFS_ERR_CHECKSUM   -10
#define EFS_ERR_QUOTA      -11
#define EFS_ERR_NOT_EMPTY  -12

typedef uint64_t efs_ino_t;
typedef uint32_t efs_export_id_t;
typedef uint32_t efs_node_id_t;

void efs_ino_path_segments(efs_ino_t ino, char seg[EFS_INO_PATH_SEGS][5]);

struct efs_node {
    efs_node_id_t id;
    char addr[64];
    uint16_t port;
    char storage_path[EFS_MAX_PATH];
    uint64_t quota; /* max bytes this node may store; 0 means unlimited */
    uint64_t used;  /* bytes currently stored on this node */
};

static inline bool efs_mode_is_dir(uint32_t mode) { return S_ISDIR(mode); }
static inline bool efs_mode_is_reg(uint32_t mode) { return S_ISREG(mode); }
static inline bool efs_mode_is_lnk(uint32_t mode) { return S_ISLNK(mode); }

const char *efs_strerror(int rc);

/* Parse a quota string. Accepts plain bytes or suffixes: T/TiB, G/GiB, M/MiB,
 * K/KiB. Returns 0 on error. */
uint64_t efs_parse_quota(const char *str);

#endif

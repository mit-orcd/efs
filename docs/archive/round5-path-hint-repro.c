#include "efs/protocol.h"
#include <stdio.h>
#define PATH_HINT_N 4096
struct path_hint_slot {
    efs_node_id_t nid;
    efs_ino_t ino;
    uint32_t ci;
    uint8_t fi;
    uint8_t path;
    uint8_t valid;
    uint8_t tried; /* sent once with no reply yet; a retry probes */
};
static struct path_hint_slot g_path_hint[PATH_HINT_N];

static uint32_t path_hint_index(efs_node_id_t nid, efs_ino_t ino, uint32_t ci,
                                uint8_t fi)
{
    uint32_t h = (uint32_t)ino * 1315423911u ^ (ci * 2654435761u) ^
                 ((uint32_t)nid << 8) ^ fi;
    return h % PATH_HINT_N;
}

static uint32_t path_hint_get(efs_node_id_t nid, efs_ino_t ino, uint32_t ci,
                              uint8_t fi)
{
    struct path_hint_slot *e =
        &g_path_hint[path_hint_index(nid, ino, ci, fi)];
    if (e->nid == nid && e->ino == ino && e->ci == ci && e->fi == fi &&
        (e->valid || e->tried)) {
        if (e->valid)
            return (uint32_t)e->path + 1u;
        return 0; /* retry of a PUT that has not been acknowledged */
    }
    /* First send of this (nid, ino, ci, fi). The server skips the
     * probe and creates on its least-queued root. */
    e->nid = nid;
    e->ino = ino;
    e->ci = ci;
    e->fi = fi;
    e->path = 0xff;
    e->valid = 0;
    e->tried = 1;
    return EFS_PATH_HINT_NEW;
}


int main(void) {
  efs_node_id_t nid=1; efs_ino_t a=1, b=4097;
  uint32_t ia=path_hint_index(nid,a,0,0), ib=path_hint_index(nid,b,0,0);
  uint32_t first=path_hint_get(nid,a,0,0);
  uint32_t collide=path_hint_get(nid,b,0,0);
  uint32_t retry=path_hint_get(nid,a,0,0);
  printf("slots=%u/%u first=%u collision=%u lost-reply-retry=%u expected-safe-probe=0\n", ia,ib,first,collide,retry);
  return ia==ib && retry==EFS_PATH_HINT_NEW ? 1 : 0;
}

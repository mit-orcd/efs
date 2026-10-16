#ifndef EFS_RAFT_FRAME_H
#define EFS_RAFT_FRAME_H
#include <stdint.h>
#include <string.h>
#include "efs/raft.h"

/* Fixed-offset readers for an encoded efs_raft_msg header (wire.h). */
static inline unsigned efs_raft_frame_rd32(const uint8_t *p)
{ return ((unsigned)p[0]<<24)|((unsigned)p[1]<<16)|((unsigned)p[2]<<8)|p[3]; }
static inline uint64_t efs_raft_frame_rd64(const uint8_t *p)
{ return ((uint64_t)efs_raft_frame_rd32(p)<<32)|efs_raft_frame_rd32(p+4); }
static inline int efs_raft_frame_group(uint8_t group)
{ return group==EFS_RAFT_GROUP_SHARD ? 0 : group==EFS_RAFT_GROUP_SHARD2 ? 1 : -1; }
/* Read probes prove only prefix zero; vote_granted avoids clearing an
 * outstanding data window. Read context, term, group and boot are retained. */
static inline void efs_raft_frame_probe(uint8_t out[80], const uint8_t in[80])
{
    memcpy(out,in,80); out[2]=1; out[3]=0;
    memset(out+36,0,24); /* last_log_term, prev_index, prev_term */
    memset(out+68,0,12); /* match_index, nentries */
}
#endif

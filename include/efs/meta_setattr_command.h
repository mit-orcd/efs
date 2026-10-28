#ifndef EFS_META_SETATTR_COMMAND_H
#define EFS_META_SETATTR_COMMAND_H
#include "efs/meta_apply.h"
#include "efs/meta_cmd.h"
#include "efs/opid.h"
#include <string.h>

/* SETATTR has the same current byte layout in the host and simulator.
 * Other simulator commands have deliberately smaller formats; do not reuse
 * their decoders merely because the operation names match. */
#define EFS_META_SETATTR_COMMAND_LEN (41u + EFS_OPID_UUID_LEN + 4u)
struct efs_meta_setattr_command {
    efs_ino_t ino;
    uint64_t now;
    struct efs_meta_setattr attrs;
    uint8_t uuid[EFS_OPID_UUID_LEN];
    uint32_t epoch;
};
static inline int efs_meta_setattr_command_decode(const uint8_t *p, uint32_t len,
                                                  struct efs_meta_setattr_command *out)
{
    if (!p || !out || len!=EFS_META_SETATTR_COMMAND_LEN || p[0]!=EFS_MD_CMD_SETATTR)
        return EFS_ERR_UNSUPPORTED;
    struct efs_meta_setattr_command q={0};
    q.ino=efs_load_be64(p+1); q.now=efs_load_be64(p+9);
    q.attrs.expect_gen=efs_load_be64(p+17); q.attrs.mask=efs_load_be32(p+25);
    q.attrs.mode=efs_load_be32(p+29); q.attrs.uid=efs_load_be32(p+33);
    q.attrs.gid=efs_load_be32(p+37);
    memcpy(q.uuid,p+41,EFS_OPID_UUID_LEN); q.epoch=efs_load_be32(p+41+EFS_OPID_UUID_LEN);
    *out=q;
    return EFS_OK;
}
static inline void efs_meta_setattr_command_encode(uint8_t *p, efs_ino_t ino,
    uint64_t now, const struct efs_meta_setattr *attrs, const uint8_t *uuid, uint32_t epoch)
{
    p[0]=EFS_MD_CMD_SETATTR;
    efs_store_be64(p+1,ino); efs_store_be64(p+9,now);
    efs_store_be64(p+17,attrs->expect_gen); efs_store_be32(p+25,attrs->mask);
    efs_store_be32(p+29,attrs->mode); efs_store_be32(p+33,attrs->uid); efs_store_be32(p+37,attrs->gid);
    memcpy(p+41,uuid,EFS_OPID_UUID_LEN); efs_store_be32(p+41+EFS_OPID_UUID_LEN,epoch);
}
#endif

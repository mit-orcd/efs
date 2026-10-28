#ifndef EFS_BYTEORDER_H
#define EFS_BYTEORDER_H
#include <stdint.h>

/* Byte-wise access is alignment-safe and independent of host endianness.
 * These primitives do not validate a buffer length; decoders do that first. */
static inline void efs_store_be32(uint8_t *p, uint32_t v)
{
    p[0]=(uint8_t)(v>>24); p[1]=(uint8_t)(v>>16);
    p[2]=(uint8_t)(v>>8); p[3]=(uint8_t)v;
}
static inline uint32_t efs_load_be32(const uint8_t *p)
{
    return (uint32_t)p[0]<<24 | (uint32_t)p[1]<<16 | (uint32_t)p[2]<<8 | p[3];
}
static inline void efs_store_be64(uint8_t *p, uint64_t v)
{
    efs_store_be32(p,(uint32_t)(v>>32)); efs_store_be32(p+4,(uint32_t)v);
}
static inline uint64_t efs_load_be64(const uint8_t *p)
{
    return (uint64_t)efs_load_be32(p)<<32 | efs_load_be32(p+4);
}
static inline void efs_store_le32(uint8_t *p, uint32_t v)
{
    p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8);
    p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24);
}
static inline uint32_t efs_load_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;
}
#endif

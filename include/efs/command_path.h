#ifndef EFS_COMMAND_PATH_H
#define EFS_COMMAND_PATH_H
/* W91 query path: one ASCII component "v3.<base64url page request>", max
 * 249 bytes; no shell parsing, JSON arguments or writes. Returned requests
 * are wire version 4 pages; callers fill scope and credentials. */
#include "efs/protocol.h"
#include <string.h>
#include "efs/query_filters.h"
/* Paged commands stream across the existing cursor protocol. Reserving the
 * full 16 MiB nonpaged limit for a small page admits only three replies under
 * the shared 64 MiB budget. Bound each command page instead; all pages and
 * close requests of one operation use the same cap, preserving token identity.
 * Long names / large limits continue through further immutable pages. */
static inline uint32_t efs_command_page_output(uint32_t operation)
{
    return operation==EFS_QUERY_STATS?16384u:1048576u;
}
/* URL-safe unpadded base64 digit of a v3. page name. */
static inline int efs_command_unbase64(unsigned char c)
{
    return c>='A' && c<='Z' ? c-'A' : c>='a' && c<='z' ? c-'a'+26 :
           c>='0' && c<='9' ? c-'0'+52 : c=='-' ? 62 : c=='_' ? 63 : -1;
}
static inline int efs_command_path_parse_v4(const char *name,struct efs_msg_query_snapshot_v4 *out)
{
    memset(out,0,sizeof(*out));
    /* Only v3. page names; the unpaged v1. and v2. names were removed. */
    if (!name || strncmp(name,"v3.",3)) return EFS_ERR_INVAL;
    size_t len=strnlen(name,256);uint8_t raw[184];unsigned bits=0,acc=0,n=0;
    if (len>249 || (len-3)%4==1) return EFS_ERR_INVAL;
    for (size_t i=3;i<len;i++) {
        int v=efs_command_unbase64(name[i]);if (v<0) return EFS_ERR_INVAL;
        acc=(acc<<6)|(unsigned)v;bits+=6;
        if (bits>=8) { bits-=8;if (n==sizeof(raw)) return EFS_ERR_INVAL;raw[n++]=(uint8_t)(acc>>bits); }
        acc&=(1u<<bits)-1;
    }
    if (acc || n<64 || n>184) return EFS_ERR_INVAL;
    out->base.version=EFS_QUERY_PAGE_VERSION;out->base.operation=raw[0];out->base.match=raw[1];
    out->limit=((uint32_t)raw[2]<<8)|raw[3];
    uint64_t values[7]={0};
    for (unsigned k=0;k<7;k++) for (unsigned j=0;j<8;j++) values[k]=(values[k]<<8)|raw[(k<2 ? 4+k*8 : 24+(k-2)*8)+j];
    out->nonce=((uint32_t)raw[20]<<24)|((uint32_t)raw[21]<<16)|((uint32_t)raw[22]<<8)|raw[23];
    out->token=values[0];out->cursor=values[1];
    out->filters.flags=(uint32_t)(values[2]>>32);out->filters.kind=(uint32_t)values[2];
    out->filters.min_size=values[3];out->filters.max_size=values[4];
    out->filters.after_ns=values[5];out->filters.before_ns=values[6];
    if (out->base.operation>EFS_QUERY_LS) return EFS_ERR_INVAL;
    if (out->base.match>3 || out->limit>10000 || !efs_query_filters_valid(&out->filters)) return EFS_ERR_INVAL;
    if (out->base.operation==EFS_QUERY_STATS && (n!=64 || out->cursor || out->limit>1 ||
        out->filters.flags || out->filters.kind || out->filters.min_size || out->filters.max_size ||
        out->filters.after_ns || out->filters.before_ns)) return EFS_ERR_INVAL;
    for (unsigned i=64;i<n;i++) {
        if (!raw[i] || raw[i]=='/') return EFS_ERR_INVAL;
        out->base.term[i-64]=(char)raw[i];
    }
    out->base.max_output=efs_command_page_output(out->base.operation);out->base.visit_budget=100000;
    return EFS_OK;
}
#endif

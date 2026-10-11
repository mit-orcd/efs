#ifndef EFS_COMMAND_PATH_H
#define EFS_COMMAND_PATH_H
/* W91 query-path v1. ASCII component, max 250 bytes; no shell parsing, JSON
 * arguments or writes. A 120-byte literal term is a deliberate initial limit.
 * Returned query requests are wire v2; callers fill scope and credentials. */
#include "efs/protocol.h"
#include <string.h>
#include "efs/query_filters.h"
#define EFS_COMMAND_MAX_TERM 120
static inline int efs_command_unhex(unsigned char c)
{
    return c>='0' && c<='9' ? c-'0' : c>='a' && c<='f' ? c-'a'+10 : -1;
}
static inline int efs_command_path_parse(const char *name, struct efs_msg_query_snapshot *out)
{
    if (!name || !out || strnlen(name,256)>255) return EFS_ERR_INVAL;
    struct efs_msg_query_snapshot q={0};
    q.version=EFS_QUERY_COMMAND_VERSION;
    q.max_output=EFS_QUERY_MAX_OUTPUT;q.visit_budget=EFS_QUERY_MAX_VISITS;
    size_t simple=0;
    if (!strncmp(name,"v1.stats",8) && (!name[8] || name[8]=='.')) {q.operation=EFS_QUERY_STATS;simple=8;}
    else if (!strncmp(name,"v1.ls",5) && (!name[5] || name[5]=='.')) {q.operation=EFS_QUERY_LS;simple=5;}
    if(simple){
        /* A per-invocation identity prevents NFS dentry/open-file caches from
         * serving a prior result. It has no effect on server query semantics. */
        if(name[simple]){
            const char *nonce=name+simple+1;
            if(strlen(nonce)!=32)return EFS_ERR_INVAL;
            for(unsigned i=0;i<32;i++)if(efs_command_unhex(nonce[i])<0)return EFS_ERR_INVAL;
        }
    }
    else {
        if (strncmp(name,"v1.find.",8) || name[8]<'0' || name[8]>'3' || name[9]!='.') return EFS_ERR_INVAL;
        q.operation=EFS_QUERY_FIND;q.match=(uint32_t)(name[8]-'0');
        const char *hex=name+10;size_t n=strlen(hex);
        if (n%2 || n>2*EFS_COMMAND_MAX_TERM || (!n && q.match!=1 && q.match!=3)) return EFS_ERR_INVAL;
        for (size_t i=0;i<n;i+=2) {
            int a=efs_command_unhex((unsigned char)hex[i]),b=efs_command_unhex((unsigned char)hex[i+1]);
            if (a<0 || b<0) return EFS_ERR_INVAL;
            unsigned char ch=(unsigned char)(a*16+b);
            if (!ch || ch=='/') return EFS_ERR_INVAL;
            q.term[i/2]=(char)ch;
        }
    }
    *out=q;return EFS_OK;
}
/* v2 filtered-find component: match.kind.flags.fixed-hex-bounds.base64url-term.
 * Unpadded canonical base64 keeps all four uint64 filters and the existing
 * 120-byte term within one 240-byte component. Old v1 paths stay unchanged. */
static inline int efs_command_unbase64(unsigned char c)
{
    return c>='A' && c<='Z' ? c-'A' : c>='a' && c<='z' ? c-'a'+26 :
           c>='0' && c<='9' ? c-'0'+52 : c=='-' ? 62 : c=='_' ? 63 : -1;
}
static inline int efs_command_path_parse_v3(const char *name,struct efs_msg_query_snapshot_v3 *out)
{
    if (!name || !out || strnlen(name,256)>255) return EFS_ERR_INVAL;
    struct efs_msg_query_snapshot_v3 q={0};
    if (strncmp(name,"v2.find.",8)) {
        int rc=efs_command_path_parse(name,&q.base);
        if (!rc) *out=q;
        return rc;
    }
    size_t len=strlen(name);
    if (len<80 || len>240 || name[8]<'0' || name[8]>'3' || name[9]!='.' ||
        name[10]<'0' || name[10]>'3' || name[11]!='.' || name[14]!='.' || name[79]!='.')
        return EFS_ERR_INVAL;
    int a=efs_command_unhex(name[12]),b=efs_command_unhex(name[13]);
    if (a<0 || b<0) return EFS_ERR_INVAL;
    q.filters.flags=(uint32_t)(a*16+b);q.filters.kind=(uint32_t)(name[10]-'0');
    uint64_t fields[4]={0};
    for (unsigned i=0;i<64;i++) {
        int v=efs_command_unhex(name[15+i]);
        if (v<0) return EFS_ERR_INVAL;
        fields[i/16]=(fields[i/16]<<4)|(unsigned)v;
    }
    q.filters.min_size=fields[0];q.filters.max_size=fields[1];
    q.filters.after_ns=fields[2];q.filters.before_ns=fields[3];
    if (!efs_query_filters_valid(&q.filters)) return EFS_ERR_INVAL;
    q.base.version=EFS_QUERY_FILTER_VERSION;q.base.operation=EFS_QUERY_FIND;
    q.base.match=(uint32_t)(name[8]-'0');q.base.max_output=EFS_QUERY_MAX_OUTPUT;
    q.base.visit_budget=EFS_QUERY_MAX_VISITS;
    uint32_t bits=0,acc=0,n=0;
    if ((len-80)%4==1) return EFS_ERR_INVAL;
    for (size_t i=80;i<len;i++) {
        int v=efs_command_unbase64(name[i]);if (v<0) return EFS_ERR_INVAL;
        acc=(acc<<6)|(unsigned)v;bits+=6;
        if (bits>=8) {
            bits-=8;unsigned char c=(unsigned char)(acc>>bits);
            if (!c || c=='/' || n>=EFS_COMMAND_MAX_TERM) return EFS_ERR_INVAL;
            q.base.term[n++]=(char)c;
        }
        acc &= (1u<<bits)-1;
    }
    if (acc || (!n && q.base.match!=1 && q.base.match!=3)) return EFS_ERR_INVAL;
    *out=q;return EFS_OK;
}
static inline int efs_command_path_parse_v4(const char *name,struct efs_msg_query_snapshot_v4 *out)
{
    memset(out,0,sizeof(*out));
    if (strncmp(name,"v3.",3)) {
        struct efs_msg_query_snapshot_v3 old;
        int rc=efs_command_path_parse_v3(name,&old);
        if (!rc) { out->base=old.base;out->filters=old.filters; }
        return rc;
    }
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
    out->base.max_output=EFS_QUERY_MAX_OUTPUT;out->base.visit_budget=100000;
    return EFS_OK;
}
#endif

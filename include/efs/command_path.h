#ifndef EFS_COMMAND_PATH_H
#define EFS_COMMAND_PATH_H
/* W91 query-path v1. ASCII component, max 250 bytes; no shell parsing, JSON
 * arguments or writes. A 120-byte literal term is a deliberate initial limit.
 * Returned query requests are wire v2; callers fill scope and credentials. */
#include "efs/protocol.h"
#include <string.h>
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
    if (!strcmp(name,"v1.stats")) q.operation=EFS_QUERY_STATS;
    else if (!strcmp(name,"v1.ls")) q.operation=EFS_QUERY_LS;
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
#endif

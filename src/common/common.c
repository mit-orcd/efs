#include "efs/common.h"
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>

void efs_ino_path_segments(efs_ino_t ino, char seg[EFS_INO_PATH_SEGS][5])
{
    uint64_t v = (uint64_t)ino;
    for (int i = 0; i < EFS_INO_PATH_SEGS; i++) {
        snprintf(seg[i], 5, "%04llu", (unsigned long long)(v % 10000ULL));
        v /= 10000ULL;
    }
}

const char *efs_strerror(int rc)
{
    switch (rc) {
    case EFS_OK:         return "success";
    case EFS_ERR_IO:     return "I/O error";
    case EFS_ERR_NOMEM:  return "out of memory";
    case EFS_ERR_NOT_FOUND: return "not found";
    case EFS_ERR_EXIST:  return "already exists";
    case EFS_ERR_INVAL:  return "invalid argument";
    case EFS_ERR_NET:    return "network error";
    case EFS_ERR_PROTO:  return "protocol error";
    case EFS_ERR_NO_QUORUM: return "no quorum";
    case EFS_ERR_DECODE: return "decode error";
    case EFS_ERR_CHECKSUM: return "checksum mismatch";
    case EFS_ERR_QUOTA:  return "quota exceeded";
    default:             return "unknown error";
    }
}

uint64_t efs_parse_quota(const char *str)
{
    if (!str || !*str)
        return 0;

    char *end = NULL;
    double val = strtod(str, &end);
    if (val < 0 || end == str)
        return 0;

    uint64_t mult = 1;
    while (*end && isspace((unsigned char)*end))
        end++;

    if (*end) {
        switch (tolower((unsigned char)*end)) {
        case 'k': mult = 1024ULL; break;
        case 'm': mult = 1024ULL * 1024; break;
        case 'g': mult = 1024ULL * 1024 * 1024; break;
        case 't': mult = 1024ULL * 1024 * 1024 * 1024; break;
        default:  return 0;
        }
    }

    return (uint64_t)(val * (double)mult);
}

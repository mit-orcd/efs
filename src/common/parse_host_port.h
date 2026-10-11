#ifndef EFS_PARSE_HOST_PORT_H
#define EFS_PARSE_HOST_PORT_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>
/* Preserve last-colon host splitting; require a complete decimal TCP port.
 * Host syntax/resolution belongs to the caller's existing network path. */
static inline int efs_parse_host_port(const char *str, char *host,
                                      size_t host_len, uint16_t *port)
{
    if (!str || !host || !port)
        return -1;
    const char *colon = strrchr(str, ':');
    if (!colon || colon == str || !colon[1])
        return -1;
    size_t hlen = (size_t)(colon - str);
    if (hlen >= host_len)
        return -1;
    uint32_t value = 0;
    for (const unsigned char *p = (const unsigned char *)colon + 1; *p; p++) {
        if (*p < '0' || *p > '9')
            return -1;
        uint32_t digit = *p - '0';
        if (value > (UINT16_MAX - digit) / 10)
            return -1;
        value = value * 10 + digit;
    }
    if (!value)
        return -1;
    memcpy(host, str, hlen);
    host[hlen] = '\0';
    *port = (uint16_t)value;
    return 0;
}
#endif

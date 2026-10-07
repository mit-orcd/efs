#include "efs/common.h"
#include "efs/protocol.h"
#include "efs/network.h"
#include "efs/version.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int parse_addr(const char *str, char *host, size_t host_len, uint16_t *port)
{
    const char *colon = strrchr(str, ':');
    if (!colon)
        return -1;
    size_t hlen = (size_t)(colon - str);
    if (hlen >= host_len)
        return -1;
    memcpy(host, str, hlen);
    host[hlen] = '\0';
    *port = (uint16_t)atoi(colon + 1);
    return 0;
}

int main(int argc, char **argv)
{
    efs_version_check_argv("efs-query", argc, argv);

    const char *addr_arg = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--raw") == 0) {
            continue;
        } else if (addr_arg == NULL) {
            addr_arg = argv[i];
        } else {
            fprintf(stderr, "Usage: %s [--raw] <server-addr:port>\n", argv[0]);
            return 1;
        }
    }
    if (addr_arg == NULL) {
        fprintf(stderr, "Usage: %s [--raw] <server-addr:port>\n", argv[0]);
        return 1;
    }

    char host[64];
    uint16_t port;
    if (parse_addr(addr_arg, host, sizeof(host), &port) != 0) {
        fprintf(stderr, "Invalid server address: %s\n", addr_arg);
        return 1;
    }

    int fd = efs_connect_tcp(host, port);
    if (fd < 0) {
        fprintf(stderr, "Cannot connect to %s:%u\n", host, port);
        return 1;
    }
    efs_set_recv_timeout(fd, EFS_IO_TIMEOUT_MS);
    efs_set_send_timeout(fd, EFS_IO_TIMEOUT_MS);

    if (efs_send_msg(fd, EFS_MSG_QUERY_STATS, NULL, 0) != 0) {
        fprintf(stderr, "Failed to send query\n");
        close(fd);
        return 1;
    }

    uint8_t type;
    void *payload = NULL;
    uint32_t payload_len = 0;
    if (efs_recv_msg(fd, &type, &payload, &payload_len) != 0 ||
        type != EFS_MSG_QUERY_STATS_REPLY) {
        fprintf(stderr, "Failed to receive query reply\n");
        free(payload);
        close(fd);
        return 1;
    }

    int result = 2;
    if (payload_len == 1 && ((const uint8_t *)payload)[0] == EFS_QUERY_STATS_UNSUPPORTED) {
        fprintf(stderr, "Logical query statistics are unavailable: server query integration is not implemented.\n");
    } else if (payload_len == sizeof(struct efs_msg_query_stats_reply)) {
        fprintf(stderr, "Logical query statistics are unavailable: legacy reply lacks supported query authority.\n");
    } else {
        fprintf(stderr, "Invalid query status reply\n");
        result = 1;
    }
    free(payload);
    close(fd);
    return result;
}

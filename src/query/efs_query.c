#include "efs/common.h"
#include "efs/protocol.h"
#include "efs/network.h"
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

static const char *format_bytes(uint64_t bytes, char *buf, size_t len)
{
    if (bytes >= 1099511627776ULL)
        snprintf(buf, len, "%.2f TiB", bytes / 1099511627776.0);
    else if (bytes >= 1073741824ULL)
        snprintf(buf, len, "%.2f GiB", bytes / 1073741824.0);
    else if (bytes >= 1048576ULL)
        snprintf(buf, len, "%.2f MiB", bytes / 1048576.0);
    else if (bytes >= 1024ULL)
        snprintf(buf, len, "%.2f KiB", bytes / 1024.0);
    else
        snprintf(buf, len, "%llu B", (unsigned long long)bytes);
    return buf;
}

int main(int argc, char **argv)
{
    int raw = 0;
    const char *addr_arg = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--raw") == 0) {
            raw = 1;
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
        type != EFS_MSG_QUERY_STATS_REPLY ||
        payload_len != sizeof(struct efs_msg_query_stats_reply)) {
        fprintf(stderr, "Failed to receive query reply\n");
        free(payload);
        close(fd);
        return 1;
    }

    struct efs_msg_query_stats_reply *reply = payload;

    if (raw) {
        printf("total_files %llu\n", (unsigned long long)reply->total_files);
        printf("total_bytes %llu\n", (unsigned long long)reply->total_bytes);
        printf("users %u\n", reply->user_count);
        for (uint32_t i = 0; i < reply->user_count; i++) {
            printf("user %u %llu %llu\n",
                   reply->users[i].uid,
                   (unsigned long long)reply->users[i].files,
                   (unsigned long long)reply->users[i].bytes);
        }
    } else {
        char bbuf[32];
        printf("Total files: %llu\n", (unsigned long long)reply->total_files);
        printf("Total bytes: %s (%llu bytes)\n",
               format_bytes(reply->total_bytes, bbuf, sizeof(bbuf)),
               (unsigned long long)reply->total_bytes);
        printf("Users:       %u\n", reply->user_count);
        if (reply->user_count > 0) {
            printf("\nPer-user breakdown:\n");
            printf("%-10s %12s %20s\n", "UID", "Files", "Bytes");
            for (uint32_t i = 0; i < reply->user_count; i++) {
                printf("%-10u %12llu %20s\n",
                       reply->users[i].uid,
                       (unsigned long long)reply->users[i].files,
                       format_bytes(reply->users[i].bytes, bbuf, sizeof(bbuf)));
            }
        }
    }

    free(payload);
    close(fd);
    return 0;
}

#ifndef EFS_SERVER_DEST_FAULT_H
#define EFS_SERVER_DEST_FAULT_H
#include "efs/common.h"
#if EFS_FAULTS
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#endif

/* B6 test hook (tests/test_b6_unavailable.py), EFS_FAULTS builds only:
 * EFS_FAULT_DEST_FILE=<path> makes this member an unavailable fragment
 * destination while <path> exists. Its first line is
 *   refuse|slow:<ms> [op,op,...]
 * refuse answers the data operation with an error (EFS_ERR_IO); slow
 * sleeps <ms> (at most 120000) and then serves it, so a client deadline
 * runs out first. The ops are put (fragment PUT), get (fragment GET),
 * commit (DATA_COMMIT), segput and segget (packed segment fragments,
 * also this member's own packer) and rebuild (W154 REBUILD_PUT); none
 * named is every one. Read on every operation, so a test turns the fault
 * on and off by writing or removing the file. Metadata (Raft) traffic is
 * never affected. Other builds always serve (0). Inline: each including
 * file keeps its own once-read path and log counter. */
static inline int server_test_dest_fault(const char *op)
{
#if EFS_FAULTS
    static const char *path;
    static int init;
    if (!__atomic_load_n(&init, __ATOMIC_ACQUIRE)) {
        path = getenv("EFS_FAULT_DEST_FILE");
        __atomic_store_n(&init, 1, __ATOMIC_RELEASE);
    }
    if (!path || !*path)
        return 0;
    char line[256] = {0};
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return 0;
    ssize_t n = read(fd, line, sizeof(line) - 1);
    close(fd);
    if (n <= 0)
        return 0;
    line[strcspn(line, "\n")] = 0;
    char *ops = strchr(line, ' ');
    if (ops) {
        *ops++ = 0;
        size_t k = strlen(op);
        int hit = 0;
        for (const char *q = ops; *q && !hit;) {
            hit = !strncmp(q, op, k) && (q[k] == ',' || q[k] == ' ' || q[k] == '\0');
            q += strcspn(q, ",");
            if (*q)
                q++;
        }
        if (!hit)
            return 0;
    }
    static uint64_t said;
    if (__atomic_add_fetch(&said, 1, __ATOMIC_RELAXED) <= 20)
        fprintf(stderr, "fault: EFS_FAULT_DEST_FILE %s %s\n", line, op);
    if (!strcmp(line, "refuse"))
        return EFS_ERR_IO;
    if (!strncmp(line, "slow:", 5)) {
        long ms = strtol(line + 5, NULL, 10);
        if (ms > 120000)
            ms = 120000;
        if (ms > 0) {
            struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
            while (nanosleep(&ts, &ts) != 0)
                ;
        }
    }
    return 0;
#else
    (void)op;
    return 0;
#endif
}

#endif

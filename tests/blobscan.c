/* blobscan: fetch a named export's metadata from a live server and analyze
 * the table composition (row counts, size sums, reachability from root).
 * Diagnostic for torn-metadata damage assessment. Not part of the test suite.
 *
 *   blobscan <host> <port> <export-name>
 */
#include "efs/common.h"
#include "efs/metadata.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static int read_full(int fd, void *buf, size_t len)
{
    char *p = buf;
    while (len > 0) {
        ssize_t r = read(fd, p, len);
        if (r <= 0)
            return -1;
        p += r;
        len -= (size_t)r;
    }
    return 0;
}

static struct efs_export *g_scan_ex;
static int cmp_row_size_desc(const void *a, const void *b)
{
    const struct efs_inode_mem *ia =
        efs_export_inode_at(g_scan_ex, *(const uint64_t *)a);
    const struct efs_inode_mem *ib =
        efs_export_inode_at(g_scan_ex, *(const uint64_t *)b);
    uint64_t sa = ia ? ia->size : 0;
    uint64_t sb = ib ? ib->size : 0;
    return sa < sb ? 1 : sa > sb ? -1 : 0;
}

static int write_full(int fd, const void *buf, size_t len)
{
    const char *p = buf;
    while (len > 0) {
        ssize_t w = write(fd, p, len);
        if (w <= 0)
            return -1;
        p += w;
        len -= (size_t)w;
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: blobscan <host> <port> <export-name>\n");
        return 2;
    }
    const char *host = argv[1];
    uint16_t port = (uint16_t)atoi(argv[2]);
    const char *name = argv[3];

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &sa.sin_addr) != 1) {
        fprintf(stderr, "bad host %s\n", host);
        return 2;
    }
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0 || connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        perror("connect");
        return 1;
    }
    struct timeval tv = { .tv_sec = 300, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    char np[256];
    memset(np, 0, sizeof(np));
    strncpy(np, name, sizeof(np) - 1);
    uint32_t olen = htonl(1 + sizeof(np));
    uint8_t type = 8; /* EFS_MSG_GET_META */
    if (write_full(fd, &olen, 4) || write_full(fd, &type, 1) ||
        write_full(fd, np, sizeof(np))) {
        perror("write");
        return 1;
    }
    uint32_t ilen_n;
    uint8_t rtype;
    if (read_full(fd, &ilen_n, 4) || read_full(fd, &rtype, 1)) {
        perror("read hdr");
        return 1;
    }
    uint32_t ilen = ntohl(ilen_n) - 1;
    fprintf(stderr, "reply type=%u len=%u (%.2f GiB)\n", rtype, ilen,
            (double)ilen / (1u << 30));
    char *buf = malloc(ilen ? ilen : 1);
    if (!buf || read_full(fd, buf, ilen)) {
        perror("read body");
        return 1;
    }
    close(fd);

    struct efs_export_root root;
    memset(&root, 0, sizeof(root));
    size_t used = 0;
    if (efs_export_root_deserialize_used(&root, buf, ilen, &used) != 0) {
        fprintf(stderr, "root parse failed\n");
        return 1;
    }
    printf("root: gen=%llu next_ino=%llu pages=%u blob_len=%u name=%s\n",
           (unsigned long long)root.generation,
           (unsigned long long)root.next_ino, root.page_count,
           root.blob_len, root.name);

    if (used >= ilen) {
        printf("no table blob in reply (mid-rebuild?)\n");
        return 0;
    }
    struct efs_export ex;
    efs_export_init(&ex, root.id, root.name);
    if (efs_export_deserialize(&ex, buf + used, ilen - used) != 0) {
        fprintf(stderr, "blob deserialize failed\n");
        return 1;
    }
    printf("table: inodes=%llu chunks=%llu next_ino=%llu\n",
           (unsigned long long)ex.inode_count,
           (unsigned long long)ex.chunk_count,
           (unsigned long long)ex.next_ino);

    /* Row composition */
    uint64_t zero_ino = 0, huge_size = 0, sum_all = 0, max_ino = 0;
    uint64_t dirs = 0, regs = 0;
    for (uint64_t i = 0; i < ex.inode_count; i++) {
        struct efs_inode_mem *in = efs_export_inode_at(&ex, i);
        if (!in)
            continue;
        if (in->ino == 0) {
            zero_ino++;
            continue;
        }
        if (efs_mode_is_dir(in->mode))
            dirs++;
        else {
            regs++;
            sum_all += in->size;
            if (in->size > (1ull << 40)) /* > 1 TiB single file */
                huge_size++;
        }
        if (in->ino > max_ino)
            max_ino = in->ino;
    }
    printf("rows: dirs=%llu files=%llu zero_ino=%llu huge_size(>1TiB)=%llu\n",
           (unsigned long long)dirs, (unsigned long long)regs,
           (unsigned long long)zero_ino, (unsigned long long)huge_size);
    printf("size sum over ALL rows: %.2f TiB\n",
           (double)sum_all / (1ull << 40));
    printf("max_ino=%llu (root next_ino=%llu)\n",
           (unsigned long long)max_ino, (unsigned long long)root.next_ino);

    /* Reachability: BFS from EFS_ROOT_INO over a parent->children map built
     * from the rows themselves (no internal index API needed). */
    uint64_t n = ex.inode_count;
    uint8_t *reach = calloc(n ? n : 1, 1);
    /* position lookup: simple open-addressing map ino -> row index */
    uint64_t cap = 1;
    while (cap < n * 2)
        cap *= 2;
    uint64_t *keys = calloc(cap, sizeof(uint64_t));
    uint64_t *vals = malloc(cap * sizeof(uint64_t));
    for (uint64_t i = 0; i < n; i++) {
        uint64_t ino = (*efs_export_inode_at(&ex, i)).ino;
        if (!ino)
            continue;
        uint64_t h = (ino * 0x9E3779B97F4A7C15ull) & (cap - 1);
        while (keys[h])
            h = (h + 1) & (cap - 1);
        keys[h] = ino;
        vals[h] = i;
    }
    /* BFS queue of row indices */
    uint64_t *q = malloc((n + 1) * sizeof(uint64_t));
    uint64_t qh = 0, qt = 0;
    uint64_t root_pos = UINT64_MAX;
    {
        uint64_t h = (EFS_ROOT_INO * 0x9E3779B97F4A7C15ull) & (cap - 1);
        while (keys[h] && keys[h] != EFS_ROOT_INO)
            h = (h + 1) & (cap - 1);
        if (keys[h] == EFS_ROOT_INO)
            root_pos = vals[h];
    }
    if (root_pos == UINT64_MAX) {
        printf("ROOT INODE MISSING from table!\n");
        return 0;
    }
    reach[root_pos] = 1;
    q[qt++] = root_pos;
    while (qh < qt) {
        uint64_t pi = q[qh++];
        uint64_t pino = (*efs_export_inode_at(&ex, pi)).ino;
        /* scan for children (O(n) per dir is too slow for 9M rows; instead
         * do one pass grouping below) — placeholder replaced below */
        (void)pino;
        break; /* replaced by sweep approach */
    }
    /* Sweep approach: repeatedly link children whose parent is reachable.
     * O(n * depth) worst case; depth is small (~10). */
    int progress = 1;
    uint64_t reach_n = 1;
    while (progress) {
        progress = 0;
        for (uint64_t i = 0; i < n; i++) {
            if (reach[i] || (*efs_export_inode_at(&ex, i)).ino == 0)
                continue;
            uint64_t par = (*efs_export_inode_at(&ex, i)).parent;
            uint64_t h = (par * 0x9E3779B97F4A7C15ull) & (cap - 1);
            while (keys[h] && keys[h] != par)
                h = (h + 1) & (cap - 1);
            if (keys[h] == par && reach[vals[h]]) {
                reach[i] = 1;
                reach_n++;
                progress = 1;
            }
        }
    }
    uint64_t unreach = 0, unreach_bytes = 0, reach_bytes = 0;
    for (uint64_t i = 0; i < n; i++) {
        if ((*efs_export_inode_at(&ex, i)).ino == 0 || efs_mode_is_dir((*efs_export_inode_at(&ex, i)).mode))
            continue;
        if (reach[i])
            reach_bytes += (*efs_export_inode_at(&ex, i)).size;
        else {
            unreach++;
            unreach_bytes += (*efs_export_inode_at(&ex, i)).size;
        }
    }
    /* Dump root's immediate children with name sanity flags. */
    printf("\nroot children (parent==1):\n");
    for (uint64_t i = 0; i < n; i++) {
        struct efs_inode_mem *in = efs_export_inode_at(&ex, i);
        if (!in->ino || in->parent != 1)
            continue;
        const char *nm = efs_export_inode_name(&ex, i);
        int empty = nm[0] == 0;
        int slash = 0, ctrl = 0;
        for (const char *p = nm; *p; p++) {
            if (*p == '/')
                slash = 1;
            if ((unsigned char)*p < 0x20)
                ctrl = 1;
        }
        printf("  ino=%llu mode=%o size=%llu name_len=%zu%s%s%s name='%s'\n",
               (unsigned long long)in->ino, in->mode,
               (unsigned long long)in->size, strlen(nm),
               empty ? " EMPTY" : "", slash ? " HAS_SLASH" : "",
               ctrl ? " HAS_CTRL" : "", empty ? "" : nm);
    }

    printf("reachable rows: %llu (%.2f TiB file data)\n",
           (unsigned long long)reach_n, (double)reach_bytes / (1ull << 40));
    printf("UNREACHABLE file rows: %llu (%.2f TiB phantom data)\n",
           (unsigned long long)unreach, (double)unreach_bytes / (1ull << 40));

    /* Top-20 reachable files by size with reconstructed paths */
    uint64_t *ord = malloc(n * sizeof(uint64_t));
    uint64_t no = 0;
    for (uint64_t i = 0; i < n; i++)
        if (reach[i] && (*efs_export_inode_at(&ex, i)).ino && !efs_mode_is_dir((*efs_export_inode_at(&ex, i)).mode))
            ord[no++] = i;
    /* sort row indices by descending size */
    g_scan_ex = &ex;
    qsort(ord, no, sizeof(uint64_t), cmp_row_size_desc);
    int nt = no < 20 ? (int)no : 20;
    printf("\ntop reachable files by size:\n");
    for (int i = 0; i < nt; i++) {
        struct efs_inode_mem *in = efs_export_inode_at(&ex, ord[i)];
        /* walk up parents for the path (depth-capped) */
        char path[1024];
        path[0] = 0;
        uint64_t cur = in->ino;
        int depth = 0;
        char comp[300];
        snprintf(comp, sizeof(comp), "/%s", efs_export_inode_name(&ex, ord[i]));
        strncat(path, comp, sizeof(path) - strlen(path) - 1);
        while (depth++ < 6) {
            uint64_t h = (cur * 0x9E3779B97F4A7C15ull) & (cap - 1);
            while (keys[h] && keys[h] != cur)
                h = (h + 1) & (cap - 1);
            if (!keys[h])
                break;
            struct efs_inode_mem *par = efs_export_inode_at(&ex, vals[h)];
            if (par->parent == par->ino || par->parent == 0)
                break;
            uint64_t pin = par->parent;
            h = (pin * 0x9E3779B97F4A7C15ull) & (cap - 1);
            while (keys[h] && keys[h] != pin)
                h = (h + 1) & (cap - 1);
            if (!keys[h])
                break;
            struct efs_inode_mem *pp = efs_export_inode_at(&ex, vals[h)];
            char tmp[1100];
            snprintf(tmp, sizeof(tmp), "/%s%s",
                     efs_export_inode_name(&ex, vals[h]), path);
            strncpy(path, tmp, sizeof(path) - 1);
            cur = pin;
        }
        printf("  %8.2f GiB  %s (ino=%llu)\n", (double)in->size / (1u << 30),
               path, (unsigned long long)in->ino);
    }
    return 0;
}

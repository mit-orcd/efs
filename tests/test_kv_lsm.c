/* Durable ordered-KV tests. No sockets, no cluster. Uses a temp directory:
 * every case that claims durability reopens the store, and the crash case
 * really exits the process so an unfsynced page cache cannot pass for it. */
#include "efs/common.h"
#include "efs/kv.h"
#include "efs/kv_key.h"
#include "efs/kv_lsm.h"
#include "efs/kv_snap.h"
#include "efs/raft.h"
#include "../src/kv/kv_lsm_internal.h"

#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures = 0;

#define CHECK(cond, msg)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);     \
            failures++;                                                       \
        }                                                                     \
    } while (0)

static char g_dir[512];

static void rmtree(const char *dir)
{
    DIR *d = opendir(dir);
    struct dirent *de;
    char p[1024];

    if (!d)
        return;
    while ((de = readdir(d))) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        snprintf(p, sizeof(p), "%s/%s", dir, de->d_name);
        unlink(p);
    }
    closedir(d);
    rmdir(dir);
}

static struct efs_kv *open_store(int sync_mode, uint32_t memtable_max)
{
    struct efs_kv_lsm_cfg cfg;

    memset(&cfg, 0, sizeof(cfg));
    cfg.sync_mode = sync_mode;
    cfg.memtable_max = memtable_max;
    cfg.l0_max = 3;
    return efs_kv_lsm_open(g_dir, &cfg);
}

static int put_s(struct efs_kv *kv, const char *k, const char *v)
{
    return efs_kv_put(kv, (const uint8_t *)k, (uint32_t)strlen(k),
                      (const uint8_t *)v, (uint32_t)strlen(v));
}

/* EFS_OK and v matches, or the rc otherwise. */
static int get_is(struct efs_kv *kv, const char *k, const char *v)
{
    uint8_t buf[256];
    uint32_t len = sizeof(buf);
    int rc = efs_kv_get(kv, (const uint8_t *)k, (uint32_t)strlen(k), buf, &len);

    if (rc != EFS_OK)
        return rc;
    if (len != strlen(v) || memcmp(buf, v, len) != 0)
        return EFS_ERR_PROTO;
    return EFS_OK;
}

struct acc {
    char buf[4096];
    int n;
};

static int acc_cb(void *user, const uint8_t *key, uint32_t klen,
                  const uint8_t *val, uint32_t vlen)
{
    struct acc *a = user;

    (void)val;
    (void)vlen;
    if (a->n + (int)klen + 2 >= (int)sizeof(a->buf))
        return EFS_ERR_INVAL;
    memcpy(a->buf + a->n, key, klen);
    a->n += (int)klen;
    a->buf[a->n++] = ',';
    a->buf[a->n] = 0;
    return 0;
}

static void test_semantics(void)
{
    struct efs_kv *kv = open_store(EFS_KV_LSM_NOSYNC, 0);
    struct efs_kv_item it[3];
    struct acc a;
    uint8_t buf[8];
    uint32_t len;

    CHECK(kv != NULL, "open");
    if (!kv)
        return;
    CHECK(put_s(kv, "b", "2") == EFS_OK, "put b");
    CHECK(put_s(kv, "a", "1") == EFS_OK, "put a");
    CHECK(put_s(kv, "c", "3") == EFS_OK, "put c");
    CHECK(get_is(kv, "a", "1") == EFS_OK, "get a");
    CHECK(get_is(kv, "zz", "") == EFS_ERR_NOT_FOUND, "get absent");
    CHECK(put_s(kv, "a", "11") == EFS_OK, "overwrite a");
    CHECK(get_is(kv, "a", "11") == EFS_OK, "get overwritten");

    /* Short buffer reports the size and does not truncate (kv.h probe). */
    len = 0;
    CHECK(efs_kv_get(kv, (const uint8_t *)"a", 1, NULL, &len) == EFS_ERR_INVAL,
          "probe rc");
    CHECK(len == 2, "probe size");
    len = 1;
    CHECK(efs_kv_get(kv, (const uint8_t *)"a", 1, buf, &len) == EFS_ERR_INVAL,
          "short rc");

    CHECK(efs_kv_del(kv, (const uint8_t *)"a", 1) == EFS_OK, "del a");
    CHECK(get_is(kv, "a", "") == EFS_ERR_NOT_FOUND, "a gone");
    CHECK(efs_kv_del(kv, (const uint8_t *)"a", 1) == EFS_ERR_NOT_FOUND,
          "del twice");

    memset(&a, 0, sizeof(a));
    CHECK(efs_kv_scan(kv, acc_cb, &a) == EFS_OK, "scan");
    CHECK(strcmp(a.buf, "b,c,") == 0, "scan order");

    /* A DEL of an absent key inside a batch is not an error. */
    it[0].op = EFS_KV_PUT;
    it[0].key = (const uint8_t *)"d";
    it[0].klen = 1;
    it[0].val = (const uint8_t *)"4";
    it[0].vlen = 1;
    it[1].op = EFS_KV_DEL;
    it[1].key = (const uint8_t *)"nope";
    it[1].klen = 4;
    it[1].val = NULL;
    it[1].vlen = 0;
    CHECK(efs_kv_batch(kv, it, 2) == EFS_OK, "batch del absent");
    CHECK(get_is(kv, "d", "4") == EFS_OK, "batch put");

    /* A DEL item's val/vlen are not part of the record and must not be
     * validated: callers fill DEL items from scan callbacks (op/key/klen
     * only) over uninitialised arrays. The memory KV never looked at them;
     * the WAL encoder's blanket vlen cap made the reaper's LANE_SWEEP fail
     * INVAL in production only (`apply lane-sweep rc=-5`). */
    it[0].op = EFS_KV_DEL;
    it[0].key = (const uint8_t *)"d";
    it[0].klen = 1;
    it[0].val = (const uint8_t *)0xdeadbeef;
    it[0].vlen = 0xffffffffu;
    CHECK(efs_kv_batch(kv, it, 1) == EFS_OK, "del with garbage vlen");
    CHECK(get_is(kv, "d", "") == EFS_ERR_NOT_FOUND, "garbage-vlen del applied");
    it[0].op = EFS_KV_PUT;
    it[0].val = (const uint8_t *)"4";
    it[0].vlen = 1;

    /* A rejected batch applies nothing. */
    it[2].op = 99;
    it[2].key = (const uint8_t *)"e";
    it[2].klen = 1;
    it[2].val = (const uint8_t *)"5";
    it[2].vlen = 1;
    it[0].key = (const uint8_t *)"e2";
    it[0].klen = 2;
    CHECK(efs_kv_batch(kv, it, 3) == EFS_ERR_INVAL, "bad op rejected");
    CHECK(get_is(kv, "e2", "4") == EFS_ERR_NOT_FOUND, "batch atomic");
    CHECK(efs_kv_batch(kv, it, 0) == EFS_OK, "empty batch");

    efs_kv_lsm_close(kv);
}

static void test_reopen_wal(void)
{
    struct efs_kv *kv;

    rmtree(g_dir);
    kv = open_store(EFS_KV_LSM_NOSYNC, 0);
    CHECK(kv != NULL, "open");
    if (!kv)
        return;
    CHECK(put_s(kv, "k1", "v1") == EFS_OK, "put");
    CHECK(put_s(kv, "k2", "v2") == EFS_OK, "put");
    CHECK(efs_kv_del(kv, (const uint8_t *)"k1", 2) == EFS_OK, "del");
    efs_kv_lsm_close(kv);

    kv = open_store(EFS_KV_LSM_NOSYNC, 0);
    CHECK(kv != NULL, "reopen");
    if (!kv)
        return;
    CHECK(get_is(kv, "k2", "v2") == EFS_OK, "replayed put");
    CHECK(get_is(kv, "k1", "") == EFS_ERR_NOT_FOUND, "replayed del");
    efs_kv_lsm_close(kv);
}

static void test_flush_and_levels(void)
{
    struct efs_kv *kv;
    uint32_t l0 = 0, l1 = 0;
    char k[32], v[32];
    int i;

    rmtree(g_dir);
    kv = open_store(EFS_KV_LSM_NOSYNC, 0);
    CHECK(kv != NULL, "open");
    if (!kv)
        return;
    for (i = 0; i < 200; i++) {
        snprintf(k, sizeof(k), "key%04d", i);
        snprintf(v, sizeof(v), "val%04d", i);
        CHECK(put_s(kv, k, v) == EFS_OK, "put");
    }
    CHECK(efs_kv_lsm_flush(kv) == EFS_OK, "flush");
    CHECK(efs_kv_lsm_seg_count(kv, &l0, &l1) == EFS_OK, "count");
    CHECK(l0 == 1 && l1 == 0, "one L0 after flush");

    /* Reads must now come out of the segment, and a tombstone written after
     * the flush has to shadow the value inside it. */
    CHECK(get_is(kv, "key0000", "val0000") == EFS_OK, "read from L0");
    CHECK(get_is(kv, "key0199", "val0199") == EFS_OK, "read from L0 last");
    CHECK(efs_kv_del(kv, (const uint8_t *)"key0100", 7) == EFS_OK, "del");
    CHECK(get_is(kv, "key0100", "") == EFS_ERR_NOT_FOUND, "tombstone shadows");
    CHECK(put_s(kv, "key0101", "new") == EFS_OK, "overwrite over L0");
    CHECK(get_is(kv, "key0101", "new") == EFS_OK, "memtable wins");

    CHECK(efs_kv_lsm_flush(kv) == EFS_OK, "flush 2");
    CHECK(efs_kv_lsm_compact(kv) == EFS_OK, "compact");
    CHECK(efs_kv_lsm_seg_count(kv, &l0, &l1) == EFS_OK, "count 2");
    CHECK(l0 == 0 && l1 == 1, "compacted into L1");
    CHECK(get_is(kv, "key0100", "") == EFS_ERR_NOT_FOUND, "tombstone applied");
    CHECK(get_is(kv, "key0101", "new") == EFS_OK, "newest kept");
    CHECK(get_is(kv, "key0000", "val0000") == EFS_OK, "old kept");

    efs_kv_lsm_close(kv);
    kv = open_store(EFS_KV_LSM_NOSYNC, 0);
    CHECK(kv != NULL, "reopen after compact");
    if (!kv)
        return;
    CHECK(efs_kv_lsm_seg_count(kv, &l0, &l1) == EFS_OK, "count 3");
    CHECK(l0 == 0 && l1 == 1, "manifest survived");
    CHECK(get_is(kv, "key0101", "new") == EFS_OK, "value survived");
    CHECK(get_is(kv, "key0100", "") == EFS_ERR_NOT_FOUND, "delete survived");
    efs_kv_lsm_close(kv);
}

/* One ordered scan must cross memtable, L0 and L1 with newest-wins. */
static void test_scan_across_levels(void)
{
    struct efs_kv *kv;
    struct acc a;

    rmtree(g_dir);
    kv = open_store(EFS_KV_LSM_NOSYNC, 0);
    CHECK(kv != NULL, "open");
    if (!kv)
        return;
    CHECK(put_s(kv, "p/a", "1") == EFS_OK, "put");
    CHECK(put_s(kv, "p/c", "1") == EFS_OK, "put");
    CHECK(put_s(kv, "q/z", "1") == EFS_OK, "put");
    CHECK(efs_kv_lsm_flush(kv) == EFS_OK, "flush");
    CHECK(efs_kv_lsm_compact(kv) == EFS_OK, "compact to L1");
    CHECK(put_s(kv, "p/b", "1") == EFS_OK, "put");
    CHECK(efs_kv_del(kv, (const uint8_t *)"p/c", 3) == EFS_OK, "del");
    CHECK(efs_kv_lsm_flush(kv) == EFS_OK, "flush to L0");
    CHECK(put_s(kv, "p/d", "1") == EFS_OK, "put in memtable");

    memset(&a, 0, sizeof(a));
    CHECK(efs_kv_scan(kv, acc_cb, &a) == EFS_OK, "scan");
    CHECK(strcmp(a.buf, "p/a,p/b,p/d,q/z,") == 0, "merged order");

    memset(&a, 0, sizeof(a));
    CHECK(efs_kv_scan_prefix(kv, (const uint8_t *)"p/", 2, acc_cb, &a) == EFS_OK,
          "scan_prefix");
    CHECK(strcmp(a.buf, "p/a,p/b,p/d,") == 0, "prefix bounded");

    /* Segments whose key range misses the prefix are skipped, not read:
     * a prefix below every L1 key, one above, and one whose only hit is
     * the segment's last key. */
    CHECK(put_s(kv, "r/a", "1") == EFS_OK, "put");
    CHECK(efs_kv_lsm_flush(kv) == EFS_OK, "flush r/ to L0");
    memset(&a, 0, sizeof(a));
    CHECK(efs_kv_scan_prefix(kv, (const uint8_t *)"o/", 2, acc_cb, &a) == EFS_OK,
          "scan below");
    CHECK(a.n == 0, "nothing below");
    memset(&a, 0, sizeof(a));
    CHECK(efs_kv_scan_prefix(kv, (const uint8_t *)"s/", 2, acc_cb, &a) == EFS_OK,
          "scan above");
    CHECK(a.n == 0, "nothing above");
    memset(&a, 0, sizeof(a));
    CHECK(efs_kv_scan_prefix(kv, (const uint8_t *)"q/", 2, acc_cb, &a) == EFS_OK,
          "scan last key");
    CHECK(strcmp(a.buf, "q/z,") == 0, "last key of the L1 segment");
    memset(&a, 0, sizeof(a));
    CHECK(efs_kv_scan_prefix(kv, (const uint8_t *)"r/", 2, acc_cb, &a) == EFS_OK,
          "scan L0 only");
    CHECK(strcmp(a.buf, "r/a,") == 0, "L0 segment past L1's range");
    memset(&a, 0, sizeof(a));
    CHECK(efs_kv_scan_from(kv, (const uint8_t *)"q/", 2, (const uint8_t *)"q/zz",
                           4, acc_cb, &a) == EFS_OK, "scan_from past last");
    CHECK(a.n == 0, "start past the segment's last key");
    efs_kv_lsm_close(kv);
}

/* scan_from is what makes a paged readdir cost the page instead of the
 * directory, so it has to resume correctly from a key that exists, from one
 * that does not, and from a tombstone — with the same answers as the
 * in-memory store, across all three source kinds (L1, L0, memtable). */
static void test_scan_from(void)
{
    struct efs_kv *kv, *mem;
    struct acc a, b;
    int i;
    static const char *const keys[] = {"p/a", "p/b", "p/c", "p/d", "p/e"};

    rmtree(g_dir);
    kv = open_store(EFS_KV_LSM_NOSYNC, 0);
    mem = efs_kv_mem_create();
    CHECK(kv != NULL && mem != NULL, "open");
    if (!kv || !mem)
        return;
    /* Spread the range over L1, L0 and the memtable. */
    CHECK(put_s(kv, "p/a", "1") == EFS_OK, "put");
    CHECK(put_s(kv, "p/c", "1") == EFS_OK, "put");
    CHECK(put_s(kv, "q/z", "1") == EFS_OK, "put");
    CHECK(efs_kv_lsm_flush(kv) == EFS_OK, "flush");
    CHECK(efs_kv_lsm_compact(kv) == EFS_OK, "compact to L1");
    CHECK(put_s(kv, "p/b", "1") == EFS_OK, "put");
    CHECK(put_s(kv, "p/x", "1") == EFS_OK, "put");
    CHECK(efs_kv_lsm_flush(kv) == EFS_OK, "flush to L0");
    CHECK(put_s(kv, "p/d", "1") == EFS_OK, "put in memtable");
    CHECK(put_s(kv, "p/e", "1") == EFS_OK, "put in memtable");
    CHECK(efs_kv_del(kv, (const uint8_t *)"p/x", 3) == EFS_OK, "del");
    for (i = 0; i < (int)(sizeof(keys) / sizeof(keys[0])); i++)
        CHECK(put_s(mem, keys[i], "1") == EFS_OK, "mem put");
    CHECK(put_s(mem, "q/z", "1") == EFS_OK, "mem put");

    /* No start is exactly scan_prefix. */
    memset(&a, 0, sizeof(a));
    CHECK(efs_kv_scan_from(kv, (const uint8_t *)"p/", 2, NULL, 0, acc_cb, &a) ==
              EFS_OK, "scan_from all");
    CHECK(strcmp(a.buf, "p/a,p/b,p/c,p/d,p/e,") == 0, "full range");

    /* Resume at a key that exists: inclusive. */
    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    CHECK(efs_kv_scan_from(kv, (const uint8_t *)"p/", 2, (const uint8_t *)"p/c",
                           3, acc_cb, &a) == EFS_OK, "scan_from existing");
    CHECK(efs_kv_scan_from(mem, (const uint8_t *)"p/", 2, (const uint8_t *)"p/c",
                           3, acc_cb, &b) == EFS_OK, "mem scan_from");
    CHECK(strcmp(a.buf, "p/c,p/d,p/e,") == 0, "resume inclusive");
    CHECK(strcmp(a.buf, b.buf) == 0, "lsm and mem agree");

    /* Resume between keys, and past a tombstone. */
    memset(&a, 0, sizeof(a));
    CHECK(efs_kv_scan_from(kv, (const uint8_t *)"p/", 2, (const uint8_t *)"p/bb",
                           4, acc_cb, &a) == EFS_OK, "scan_from gap");
    CHECK(strcmp(a.buf, "p/c,p/d,p/e,") == 0, "resume at absent key");
    memset(&a, 0, sizeof(a));
    CHECK(efs_kv_scan_from(kv, (const uint8_t *)"p/", 2, (const uint8_t *)"p/x",
                           3, acc_cb, &a) == EFS_OK, "scan_from tombstone");
    CHECK(a.n == 0, "tombstone not emitted on resume");

    /* Start outside the prefix: before yields everything, after yields none.
     * The prefix still bounds the answer, so q/z never appears. */
    memset(&a, 0, sizeof(a));
    CHECK(efs_kv_scan_from(kv, (const uint8_t *)"p/", 2, (const uint8_t *)"a", 1,
                           acc_cb, &a) == EFS_OK, "scan_from before");
    CHECK(strcmp(a.buf, "p/a,p/b,p/c,p/d,p/e,") == 0, "start before prefix");
    memset(&a, 0, sizeof(a));
    CHECK(efs_kv_scan_from(kv, (const uint8_t *)"p/", 2, (const uint8_t *)"z", 1,
                           acc_cb, &a) == EFS_OK, "scan_from after");
    CHECK(a.n == 0, "start past prefix");

    /* Paging one entry at a time must visit each key exactly once. Resuming
     * strictly after key K means starting at K with a 0 byte appended: that
     * is the immediate successor of K and precedes anything longer that
     * begins with K, so no entry can be skipped or repeated. */
    {
        uint8_t next[64];
        uint32_t nlen = 0;
        char seen[256];
        int n = 0, guard;

        seen[0] = 0;
        for (guard = 0; guard < 16; guard++) {
            uint32_t klen;

            memset(&a, 0, sizeof(a));
            CHECK(efs_kv_scan_from(kv, (const uint8_t *)"p/", 2,
                                   nlen ? next : NULL, nlen, acc_cb, &a) ==
                      EFS_OK, "page");
            if (a.n == 0)
                break;
            klen = (uint32_t)strcspn(a.buf, ",");
            a.buf[klen] = 0; /* first key of this page */
            strcat(seen, a.buf);
            strcat(seen, ",");
            n++;
            memcpy(next, a.buf, klen);
            next[klen] = 0;
            nlen = klen + 1;
        }
        CHECK(n == 5, "paged the whole range");
        CHECK(strcmp(seen, "p/a,p/b,p/c,p/d,p/e,") == 0, "each key once");
    }

    efs_kv_mem_free(mem);
    efs_kv_lsm_close(kv);
}

/* Recovery must remove the discarded bytes, not merely stop replay: a new
 * acknowledged record must still be reachable after a second process crash. */
static void test_torn_tail(void)
{
    static const uint8_t tails[][16] = {
        {0xaa, 0xbb, 0xcc},                         /* short header */
        {0, 0, 0, 0, 0, 0, 0, 0},                /* reserved zeros */
        {0xaa, 0xbb, 0xcc, 0xdd, 9, 0, 0, 0, 1}, /* short payload */
        {0xaa, 0xbb, 0xcc, 0xdd, 4, 0, 0, 0, 1, 0, 0, 0} /* bad CRC */
    };
    static const size_t lengths[] = {3, 8, 9, 12};
    for (unsigned k = 0; k < sizeof(lengths) / sizeof(lengths[0]); ++k) {
        rmtree(g_dir);
        struct efs_kv *kv = open_store(EFS_KV_LSM_SYNC, 0);
        CHECK(kv != NULL, "open tail fixture");
        if (!kv) return;
        CHECK(put_s(kv, "good", "yes") == EFS_OK, "durable prefix");
        efs_kv_lsm_close(kv);
        /* A normal reopen also discards any reserved zero EOF. */
        kv = open_store(EFS_KV_LSM_SYNC, 0);
        CHECK(kv && get_is(kv, "good", "yes") == EFS_OK, "normal reserved EOF recovery");
        if (!kv) return;
        efs_kv_lsm_close(kv);
        char path[600]; struct stat before, after;
        snprintf(path, sizeof(path), "%s/wal", g_dir);
        CHECK(!stat(path, &before), "prefix length");
        FILE *f = fopen(path, "ab");
        CHECK(f != NULL, "open append invalid tail");
        if (!f) return;
        CHECK(fwrite(tails[k], 1, lengths[k], f) == lengths[k], "append tail");
        CHECK(!fclose(f), "close invalid tail");
        pid_t child = fork();
        CHECK(child >= 0, "fork recovery writer");
        if (child == 0) {
            kv = open_store(EFS_KV_LSM_SYNC, 0);
            if (!kv || get_is(kv, "good", "yes") != EFS_OK ||
                stat(path, &after) || after.st_size != before.st_size ||
                put_s(kv, "after", "acknowledged") != EFS_OK) _exit(2);
            _exit(0); /* No destructor or compaction may mask the WAL defect. */
        }
        int status = 0;
        if (child < 0) return;
        CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
              WEXITSTATUS(status) == 0, "repair and durable append before crash");
        kv = open_store(EFS_KV_LSM_SYNC, 0);
        CHECK(kv != NULL, "second recovery");
        if (!kv) return;
        CHECK(get_is(kv, "good", "yes") == EFS_OK, "prefix survived twice");
        CHECK(get_is(kv, "after", "acknowledged") == EFS_OK,
              "post-repair acknowledged write survived second recovery");
        CHECK(put_s(kv, "third", "restart") == EFS_OK, "append after second recovery");
        efs_kv_lsm_close(kv);
        kv = open_store(EFS_KV_LSM_SYNC, 0);
        CHECK(kv && get_is(kv, "third", "restart") == EFS_OK,
              "third recovery retains later append");
        if (kv) efs_kv_lsm_close(kv);
    }
}

/* Real durability: the child exits without unwinding after batch returned,
 * so only an fsync that already happened can make the parent see the write. */
static void test_crash_durability(void)
{
    pid_t pid;
    int status = 0;
    struct efs_kv *kv;

    rmtree(g_dir);
    pid = fork();
    CHECK(pid >= 0, "fork");
    if (pid < 0)
        return;
    if (pid == 0) {
        struct efs_kv *c = open_store(EFS_KV_LSM_SYNC, 0);
        if (!c)
            _exit(2);
        if (put_s(c, "durable", "1") != EFS_OK)
            _exit(3);
        if (put_s(c, "durable2", "2") != EFS_OK)
            _exit(4);
        _exit(0);
    }
    CHECK(waitpid(pid, &status, 0) == pid, "wait");
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "child ok");
    kv = open_store(EFS_KV_LSM_SYNC, 0);
    CHECK(kv != NULL, "reopen after crash");
    if (!kv)
        return;
    CHECK(get_is(kv, "durable", "1") == EFS_OK, "survived crash");
    CHECK(get_is(kv, "durable2", "2") == EFS_OK, "survived crash 2");
    efs_kv_lsm_close(kv);
}

/* Enough volume to force several flushes and a compaction on its own, then
 * verify every key through a reopen. */
static void test_bulk_auto_compact(void)
{
    struct efs_kv *kv;
    uint32_t l0 = 0, l1 = 0;
    char k[32], v[64];
    int i;
    int bad = 0;

    rmtree(g_dir);
    kv = open_store(EFS_KV_LSM_NOSYNC, 32 * 1024);
    CHECK(kv != NULL, "open");
    if (!kv)
        return;
    for (i = 0; i < 4000; i++) {
        snprintf(k, sizeof(k), "bulk/%06d", i);
        snprintf(v, sizeof(v), "value-for-%06d-padding-padding", i);
        if (put_s(kv, k, v) != EFS_OK)
            bad++;
    }
    CHECK(bad == 0, "bulk puts");
    for (i = 0; i < 4000; i += 3) {
        snprintf(k, sizeof(k), "bulk/%06d", i);
        if (efs_kv_del(kv, (const uint8_t *)k, (uint32_t)strlen(k)) != EFS_OK)
            bad++;
    }
    CHECK(bad == 0, "bulk deletes");
    CHECK(efs_kv_lsm_quiesce(kv) == EFS_OK, "quiesce");
    CHECK(efs_kv_lsm_seg_count(kv, &l0, &l1) == EFS_OK, "count");
    CHECK(l1 > 0, "auto compaction ran");
    efs_kv_lsm_close(kv);

    kv = open_store(EFS_KV_LSM_NOSYNC, 32 * 1024);
    CHECK(kv != NULL, "reopen");
    if (!kv)
        return;
    for (i = 0; i < 4000; i++) {
        snprintf(k, sizeof(k), "bulk/%06d", i);
        snprintf(v, sizeof(v), "value-for-%06d-padding-padding", i);
        if (i % 3 == 0) {
            if (get_is(kv, k, v) != EFS_ERR_NOT_FOUND)
                bad++;
        } else if (get_is(kv, k, v) != EFS_OK) {
            bad++;
        }
    }
    CHECK(bad == 0, "all keys correct after reopen");
    efs_kv_lsm_close(kv);
}

static int put_ino(struct efs_kv *kv, uint32_t shard, efs_ino_t ino,
                   const char *v)
{
    uint8_t key[EFS_KV_KEY_MAX];
    uint32_t klen = 0;

    CHECK(efs_kv_key_inode(shard, ino, key, &klen) == EFS_OK, "key");
    return efs_kv_put(kv, key, klen, (const uint8_t *)v, (uint32_t)strlen(v));
}

static int get_ino(struct efs_kv *kv, uint32_t shard, efs_ino_t ino,
                   const char *v)
{
    uint8_t key[EFS_KV_KEY_MAX], buf[64];
    uint32_t klen = 0, vlen = sizeof(buf);
    int rc;

    CHECK(efs_kv_key_inode(shard, ino, key, &klen) == EFS_OK, "key");
    rc = efs_kv_get(kv, key, klen, buf, &vlen);
    if (!v)
        return rc;
    if (rc != EFS_OK)
        return rc;
    if (vlen != strlen(v) || memcmp(buf, v, vlen) != 0)
        return EFS_ERR_PROTO;
    return EFS_OK;
}

/* Group export is the existing WAL item payload, filtered by shard→group.
 * Import replaces that group's namespace and leaves the other group alone. */
static void test_kv_group_snap(void)
{
    struct efs_kv *src = efs_kv_mem_create();
    struct efs_kv *dst = efs_kv_mem_create();
    uint8_t *blob = NULL;
    uint32_t blen = 0;

    CHECK(src && dst, "mem kv");
    const uint8_t local_key[]={255,255,'r','a','f','t',0};
    CHECK(efs_kv_put(src,local_key,sizeof(local_key),(const uint8_t *)"source",6)==EFS_OK,"source local cursor");
    CHECK(efs_kv_put(dst,local_key,sizeof(local_key),(const uint8_t *)"target",6)==EFS_OK,"target local cursor");
    CHECK(put_ino(src, 1, 1, "odd") == EFS_OK, "put g0");
    CHECK(put_ino(src, 2, 2, "even") == EFS_OK, "put g2");
    CHECK(efs_kv_group_export(src, EFS_RAFT_GROUP_SHARD, 4096, &blob, &blen) ==
              EFS_OK,
          "export g0");
    CHECK(blen >= 4 && blob, "blob");
    CHECK(efs_kv_group_import(dst, EFS_RAFT_GROUP_SHARD, blob, blen) == EFS_OK,
          "import g0");
    CHECK(get_ino(dst, 1, 1, "odd") == EFS_OK, "g0 landed");
    uint8_t local_value[16];uint32_t local_length=sizeof(local_value);
    CHECK(efs_kv_get(dst,local_key,sizeof(local_key),local_value,&local_length)==EFS_OK &&
          local_length==6 && !memcmp(local_value,"target",6),"group import neither imports nor erases node-local cursor");
    CHECK(get_ino(dst, 2, 2, NULL) == EFS_ERR_NOT_FOUND, "g2 not in g0 snap");
    CHECK(put_ino(dst, 1, 1, "ODD") == EFS_OK, "overwrite");
    CHECK(efs_kv_group_import(dst, EFS_RAFT_GROUP_SHARD, blob, blen) == EFS_OK,
          "reimport same image");
    CHECK(get_ino(dst, 1, 1, "odd") == EFS_OK, "changed value restored");
    free(blob);
    blob = NULL;
    CHECK(put_ino(dst, 1, 99, "stale") == EFS_OK, "stale");
    CHECK(efs_kv_group_export(src, EFS_RAFT_GROUP_SHARD, 4096, &blob, &blen) ==
              EFS_OK,
          "export g0 again");
    CHECK(efs_kv_group_import(dst, EFS_RAFT_GROUP_SHARD, blob, blen) == EFS_OK,
          "replace");
    CHECK(get_ino(dst, 1, 99, NULL) == EFS_ERR_NOT_FOUND, "stale deleted");
    CHECK(get_ino(dst, 1, 1, "odd") == EFS_OK, "live kept");
    free(blob);
    blob = NULL;
    /* Enough dest-only keys that the collect arena reallocs. Pointers into
     * the old arena used to reach efs_kv_batch and smash the LSM heap. */
    {
        uint32_t i;

        for (i = 3; i < 2000; i += 2)
            CHECK(put_ino(dst, 1, (efs_ino_t)i, "stale") == EFS_OK, "arena stale");
        CHECK(efs_kv_group_export(src, EFS_RAFT_GROUP_SHARD, 65536, &blob,
                                  &blen) == EFS_OK,
              "export arena");
        CHECK(efs_kv_group_import(dst, EFS_RAFT_GROUP_SHARD, blob, blen) ==
                  EFS_OK,
              "import arena");
        CHECK(get_ino(dst, 1, 1, "odd") == EFS_OK, "live after arena");
        CHECK(get_ino(dst, 1, 3, NULL) == EFS_ERR_NOT_FOUND, "first stale gone");
        CHECK(get_ino(dst, 1, 1999, NULL) == EFS_ERR_NOT_FOUND, "last stale gone");
        free(blob);
        blob = NULL;
    }
    {
        struct efs_kv *lsm;
        char saved[512];
        uint32_t i;

        memcpy(saved, g_dir, sizeof(saved));
        snprintf(g_dir, sizeof(g_dir), "/tmp/efs-kvlsm-snap-%d", (int)getpid());
        rmtree(g_dir);
        lsm = open_store(EFS_KV_LSM_NOSYNC, 0);
        CHECK(lsm != NULL, "lsm snap");
        if (lsm) {
            for (i = 3; i < 2000; i += 2)
                CHECK(put_ino(lsm, 1, (efs_ino_t)i, "stale") == EFS_OK,
                      "lsm stale");
            CHECK(efs_kv_group_export(src, EFS_RAFT_GROUP_SHARD, 65536, &blob,
                                      &blen) == EFS_OK,
                  "export lsm");
            CHECK(efs_kv_group_import(lsm, EFS_RAFT_GROUP_SHARD, blob, blen) ==
                      EFS_OK,
                  "import lsm");
            CHECK(get_ino(lsm, 1, 1, "odd") == EFS_OK, "lsm live");
            CHECK(get_ino(lsm, 1, 3, NULL) == EFS_ERR_NOT_FOUND, "lsm stale gone");
            efs_kv_lsm_close(lsm);
        }
        rmtree(g_dir);
        memcpy(g_dir, saved, sizeof(g_dir));
        free(blob);
        blob = NULL;
    }
    CHECK(efs_kv_group_export(src, EFS_RAFT_GROUP_SHARD, 8, &blob, &blen) ==
              EFS_ERR_BUSY,
          "oversize is BUSY");
    efs_kv_mem_free(src);
    efs_kv_mem_free(dst);
}

/* Host snapshot calls efs_kv_lsm_flush, which used to only append L0.
 * At KV_LSM_MAX_SEGS the next write compact-crashes (fcstor004 SIGSEGV
 * in compact_emit / kv_seg_w_bytes). The smash is 64 L0 + 1 overlapping
 * L1 written into drop[64]. */
static void test_compact_full_l0(void)
{
    struct efs_kv_lsm_cfg cfg;
    struct efs_kv *kv;
    uint32_t l0 = 0, l1 = 0;
    char k[16], v[16];
    int i, bad = 0;

    rmtree(g_dir);
    memset(&cfg, 0, sizeof(cfg));
    cfg.sync_mode = EFS_KV_LSM_NOSYNC;
    cfg.memtable_max = 4u * 1024u * 1024u;
    /* Above the engine cap so the background compactor does not race the
     * "L0 is full" check. The cap itself (64) is what this test fills. */
    cfg.l0_max = 1000;
    kv = efs_kv_lsm_open(g_dir, &cfg);
    CHECK(kv != NULL, "open full-l0");
    if (!kv)
        return;
    for (i = 0; i < 64; i++) {
        snprintf(k, sizeof(k), "f%02d", i);
        snprintf(v, sizeof(v), "v%02d", i);
        if (put_s(kv, k, v) != EFS_OK || efs_kv_lsm_flush(kv) != EFS_OK)
            bad++;
    }
    CHECK(bad == 0, "64 explicit flushes");
    CHECK(efs_kv_lsm_seg_count(kv, &l0, &l1) == EFS_OK, "count at cap");
    CHECK(l0 == 64, "L0 at MAX_SEGS");
    CHECK(efs_kv_lsm_compact(kv) == EFS_OK, "compact 64 L0");
    CHECK(efs_kv_lsm_seg_count(kv, &l0, &l1) == EFS_OK, "count after compact");
    CHECK(l0 == 0 && l1 >= 1, "64 L0 became L1");
    bad = 0;
    for (i = 0; i < 64; i++) {
        snprintf(k, sizeof(k), "f%02d", i);
        snprintf(v, sizeof(v), "w%02d", i);
        if (put_s(kv, k, v) != EFS_OK || efs_kv_lsm_flush(kv) != EFS_OK)
            bad++;
    }
    CHECK(bad == 0, "second 64 L0 over L1");
    CHECK(efs_kv_lsm_seg_count(kv, &l0, &l1) == EFS_OK, "count at 64+L1");
    CHECK(l0 == 64 && l1 >= 1, "full L0 plus overlapping L1");
    CHECK(efs_kv_lsm_compact(kv) == EFS_OK, "compact 64 L0 + L1");
    CHECK(efs_kv_lsm_seg_count(kv, &l0, &l1) == EFS_OK, "count after 2nd");
    CHECK(l0 == 0 && l1 >= 1, "second compact emptied L0");
    bad = 0;
    for (i = 0; i < 64; i++) {
        snprintf(k, sizeof(k), "f%02d", i);
        snprintf(v, sizeof(v), "w%02d", i);
        if (get_is(kv, k, v) != EFS_OK)
            bad++;
    }
    CHECK(bad == 0, "keys survived 64-L0 + L1 compact");
    efs_kv_lsm_close(kv);
}

/* L1 used to be a 64-slot array. Publish returned BUSY at the 65th file
 * and the MANIFEST loader refused to open it. The on-disk list is one
 * line per file; 80 ranges must compact, reopen, and read back. */
static void test_l1_grows(void)
{
    struct efs_kv_lsm_cfg cfg;
    struct efs_kv_lsm_view *view = NULL;
    struct efs_kv *kv;
    uint32_t l0 = 0, l1 = 0;
    char k[4], v[8];
    uint8_t got[8];
    uint32_t glen;
    int i, bad = 0, n = 80;

    rmtree(g_dir);
    memset(&cfg, 0, sizeof(cfg));
    cfg.sync_mode = EFS_KV_LSM_NOSYNC;
    cfg.memtable_max = 4u * 1024u * 1024u;
    cfg.l0_max = 1000;
    kv = efs_kv_lsm_open(g_dir, &cfg);
    CHECK(kv != NULL, "open l1 grow");
    if (!kv)
        return;
    for (i = 0; i < n; i++) {
        k[0] = (char)(i + 1);
        k[1] = 'k';
        k[2] = 0;
        snprintf(v, sizeof(v), "v%02d", i);
        if (put_s(kv, k, v) != EFS_OK || efs_kv_lsm_flush(kv) != EFS_OK ||
            efs_kv_lsm_compact(kv) != EFS_OK)
            bad++;
    }
    CHECK(bad == 0, "80 range flush+compact");
    CHECK(efs_kv_lsm_seg_count(kv, &l0, &l1) == EFS_OK && l0 == 0 &&
              l1 == (uint32_t)n,
          "L1 past 64");
    CHECK(efs_kv_lsm_view_pin(kv, &view) == EFS_OK && view, "pin past 64");
    if (view) {
        k[0] = 1;
        k[1] = 'k';
        k[2] = 0;
        glen = sizeof(got);
        CHECK(efs_kv_lsm_view_get(view, (const uint8_t *)k, 2, got, &glen) ==
                  EFS_OK &&
              glen == 3 && memcmp(got, "v00", 3) == 0,
              "pinned view sees every segment");
        efs_kv_lsm_view_unpin(view);
    }
    efs_kv_lsm_close(kv);
    kv = efs_kv_lsm_open(g_dir, &cfg);
    CHECK(kv != NULL, "reopen past 64 L1");
    if (!kv)
        return;
    CHECK(efs_kv_lsm_seg_count(kv, &l0, &l1) == EFS_OK && l0 == 0 &&
              l1 == (uint32_t)n,
          "manifest kept every L1");
    bad = 0;
    for (i = 0; i < n; i++) {
        k[0] = (char)(i + 1);
        k[1] = 'k';
        k[2] = 0;
        snprintf(v, sizeof(v), "v%02d", i);
        if (get_is(kv, k, v) != EFS_OK)
            bad++;
    }
    CHECK(bad == 0, "keys live past 64 L1");
    efs_kv_lsm_close(kv);
}

/* 49 L0 files: a one-run flush still fits, but the pump must not walk
 * the memtable to discover that. nowait refuses; the waiting flush
 * still places the run. */
static void test_flush_nowait(void)
{
    struct efs_kv_lsm_cfg cfg;
    struct efs_kv *kv;
    uint32_t l0 = 0, l1 = 0;
    char k[8], v[8];
    int i, bad = 0;

    rmtree(g_dir);
    memset(&cfg, 0, sizeof(cfg));
    cfg.sync_mode = EFS_KV_LSM_NOSYNC;
    cfg.memtable_max = 4u * 1024u * 1024u;
    cfg.l0_max = 1000;
    kv = efs_kv_lsm_open(g_dir, &cfg);
    CHECK(kv != NULL, "open nowait");
    if (!kv)
        return;
    for (i = 0; i < 49; i++) {
        snprintf(k, sizeof(k), "n%02d", i);
        snprintf(v, sizeof(v), "v%02d", i);
        if (put_s(kv, k, v) != EFS_OK || efs_kv_lsm_flush(kv) != EFS_OK)
            bad++;
    }
    CHECK(bad == 0, "49 flushes");
    CHECK(efs_kv_lsm_seg_count(kv, &l0, &l1) == EFS_OK && l0 == 49,
          "L0 at the nowait line");
    CHECK(put_s(kv, "n49", "v49") == EFS_OK, "one more key");
    CHECK(efs_kv_lsm_flush(kv) == EFS_OK, "one run still fits");
    CHECK(efs_kv_lsm_seg_count(kv, &l0, &l1) == EFS_OK && l0 == 50,
          "waiting flush published");
    CHECK(efs_kv_lsm_flush_nowait(kv) == EFS_OK, "empty memtable is OK");
    CHECK(put_s(kv, "n50", "v50") == EFS_OK, "key past the line");
    /* D10: admission is L0 bytes (1 GiB), not a file count. 51 tiny files
     * are far under that, so the nowait flush places the run. */
    CHECK(efs_kv_lsm_flush_nowait(kv) == EFS_OK, "nowait under the byte cap");
    CHECK(efs_kv_lsm_seg_count(kv, &l0, &l1) == EFS_OK && l0 == 51,
          "nowait flush published");
    efs_kv_lsm_close(kv);
}

/* Hold writes 32 records then one fsync; reopen must see them all. */
static void test_sync_hold(void)
{
    struct efs_kv *kv;
    char k[16], v[16];
    int i, bad = 0;

    rmtree(g_dir);
    kv = open_store(EFS_KV_LSM_SYNC, 0);
    CHECK(kv != NULL, "open hold");
    if (!kv)
        return;
    CHECK(efs_kv_lsm_sync_hold(kv) == EFS_OK, "hold");
    for (i = 0; i < 32; i++) {
        snprintf(k, sizeof(k), "h%02d", i);
        snprintf(v, sizeof(v), "v%02d", i);
        if (put_s(kv, k, v) != EFS_OK)
            bad++;
        /* A later put in the same hold must see the earlier one. */
        if (i > 0) {
            snprintf(k, sizeof(k), "h%02d", i - 1);
            snprintf(v, sizeof(v), "v%02d", i - 1);
            if (get_is(kv, k, v) != EFS_OK)
                bad++;
        }
    }
    CHECK(bad == 0, "32 puts under hold");
    CHECK(efs_kv_lsm_sync_release(kv) == EFS_OK, "release");
    efs_kv_lsm_close(kv);
    kv = open_store(EFS_KV_LSM_SYNC, 0);
    CHECK(kv != NULL, "reopen hold");
    if (!kv)
        return;
    bad = 0;
    for (i = 0; i < 32; i++) {
        snprintf(k, sizeof(k), "h%02d", i);
        snprintf(v, sizeof(v), "v%02d", i);
        if (get_is(kv, k, v) != EFS_OK)
            bad++;
    }
    CHECK(bad == 0, "hold fsync survived reopen");
    efs_kv_lsm_close(kv);
}

static void *view_worker(void *arg)
{
    struct efs_kv *kv = arg;
    int i, rc;

    for (i = 0; i < 20; i++) {
        char k[32], v[32];

        snprintf(k, sizeof(k), "extra%02d", i);
        snprintf(v, sizeof(v), "x%02d", i);
        if (put_s(kv, k, v) != EFS_OK)
            return (void *)1;
        if (efs_kv_lsm_flush(kv) != EFS_OK)
            return (void *)1;
        /* BUSY = under the 1/8 rule and under the file cap: nothing to
         * merge yet. That is the compactor's answer, not a failure. */
        rc = efs_kv_lsm_compact(kv);
        if (rc != EFS_OK && rc != EFS_ERR_BUSY)
            return (void *)1;
    }
    return NULL;
}

/* A pinned view keeps reading the segments it pinned while another thread
 * flushes and compacts. */
static void test_pinned_view(void)
{
    struct efs_kv *kv;
    struct efs_kv_lsm_view *view = NULL;
    pthread_t th;
    uint8_t buf[32];
    uint32_t n;
    int i, bad = 0;
    void *wr = NULL;

    rmtree(g_dir);
    kv = open_store(EFS_KV_LSM_NOSYNC, 0);
    CHECK(kv != NULL, "open view");
    if (!kv)
        return;
    CHECK(put_s(kv, "pinned", "old") == EFS_OK, "put pinned");
    CHECK(efs_kv_lsm_flush(kv) == EFS_OK, "flush pinned");
    CHECK(efs_kv_lsm_view_pin(kv, &view) == EFS_OK, "pin");
    CHECK(pthread_create(&th, NULL, view_worker, kv) == 0, "worker");
    for (i = 0; i < 100; i++) {
        n = sizeof(buf);
        if (efs_kv_lsm_view_get(view, (const uint8_t *)"pinned", 6, buf, &n) !=
                EFS_OK ||
            n != 3 || memcmp(buf, "old", 3) != 0)
            bad++;
    }
    CHECK(bad == 0, "view stable across compaction");
    CHECK(pthread_join(th, &wr) == 0, "join");
    CHECK(wr == NULL, "worker ok");
    n = sizeof(buf);
    CHECK(efs_kv_lsm_view_get(view, (const uint8_t *)"extra00", 7, buf, &n) ==
              EFS_ERR_NOT_FOUND,
          "view does not see later keys");
    CHECK(get_is(kv, "pinned", "old") == EFS_OK, "live still has pinned");
    CHECK(get_is(kv, "extra00", "x00") == EFS_OK, "live sees later key");
    efs_kv_lsm_view_unpin(view);
    efs_kv_lsm_close(kv);
}

/* Kill after the compaction output file is durable and before the manifest
 * rename. Reopen must still serve every key from the old segments. */
static void test_compact_crash(void)
{
    struct efs_kv *kv;
    pid_t pid;
    int status = -1;
    int i, bad = 0;

    rmtree(g_dir);
    kv = open_store(EFS_KV_LSM_SYNC, 0);
    CHECK(kv != NULL, "open crash-compact");
    if (!kv)
        return;
    for (i = 0; i < 20; i++) {
        char k[32], v[32];

        snprintf(k, sizeof(k), "ck%02d", i);
        snprintf(v, sizeof(v), "cv%02d", i);
        CHECK(put_s(kv, k, v) == EFS_OK, "put ck");
    }
    CHECK(efs_kv_lsm_flush(kv) == EFS_OK, "flush ck");
    efs_kv_lsm_close(kv);
    setenv("EFS_KV_COMPACT_DIE", "1", 1);
    pid = fork();
    CHECK(pid >= 0, "fork compact");
    if (pid == 0) {
        kv = open_store(EFS_KV_LSM_SYNC, 0);
        if (!kv)
            _exit(2);
        if (efs_kv_lsm_compact(kv) == EFS_OK)
            _exit(0);
        _exit(3);
    }
    unsetenv("EFS_KV_COMPACT_DIE");
    if (pid > 0) {
        waitpid(pid, &status, 0);
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 99, "died after segment");
    }
    kv = open_store(EFS_KV_LSM_SYNC, 0);
    CHECK(kv != NULL, "reopen after compact crash");
    if (!kv)
        return;
    for (i = 0; i < 20; i++) {
        char k[32], v[32];

        snprintf(k, sizeof(k), "ck%02d", i);
        snprintf(v, sizeof(v), "cv%02d", i);
        if (get_is(kv, k, v) != EFS_OK)
            bad++;
    }
    CHECK(bad == 0, "keys survived compact crash");
    efs_kv_lsm_close(kv);
}

/* A flush of two key[0] ranges is two L0 files. Compacting one range
 * rewrites that range's L1 and leaves the other file in place. */
static int sst_names(char names[][64], int cap)
{
    DIR *d = opendir(g_dir);
    struct dirent *de;
    int n = 0;

    if (!d)
        return 0;
    while ((de = readdir(d))) {
        if (strncmp(de->d_name, "seg-1-", 6) != 0)
            continue;
        if (n < cap)
            snprintf(names[n], 64, "%.63s", de->d_name);
        n++;
    }
    closedir(d);
    return n;
}

static void test_partitioned_flush(void)
{
    struct efs_kv_lsm_cfg cfg;
    struct efs_kv *kv;
    uint32_t l0 = 0, l1 = 0;
    uint8_t ka[8], kb[8], kc[8], buf[4];
    uint32_t len;
    char before[8][64], after[8][64];
    int nb, na, i, kept = 0;

    rmtree(g_dir);
    memset(&cfg, 0, sizeof(cfg));
    cfg.sync_mode = EFS_KV_LSM_NOSYNC;
    cfg.l0_max = 1000;
    kv = efs_kv_lsm_open(g_dir, &cfg);
    CHECK(kv != NULL, "open partitioned");
    if (!kv)
        return;
    memset(ka, 0x01, sizeof(ka));
    memset(kb, 0x80, sizeof(kb));
    memset(kc, 0x01, sizeof(kc));
    ka[7] = 1;
    kb[7] = 2;
    kc[7] = 3;
    CHECK(efs_kv_put(kv, ka, 8, (const uint8_t *)"A", 1) == EFS_OK, "put A");
    CHECK(efs_kv_put(kv, kb, 8, (const uint8_t *)"B", 1) == EFS_OK, "put B");
    CHECK(efs_kv_lsm_flush(kv) == EFS_OK, "flush two ranges");
    CHECK(efs_kv_lsm_seg_count(kv, &l0, &l1) == EFS_OK, "count flush");
    CHECK(l0 == 2 && l1 == 0, "one L0 file per range");
    CHECK(efs_kv_lsm_compact(kv) == EFS_OK, "compact first range");
    CHECK(efs_kv_lsm_seg_count(kv, &l0, &l1) == EFS_OK, "count 1");
    CHECK(l0 == 1 && l1 == 1, "one range stayed L0");
    CHECK(efs_kv_lsm_compact(kv) == EFS_OK, "compact second range");
    CHECK(efs_kv_lsm_seg_count(kv, &l0, &l1) == EFS_OK, "count 2");
    CHECK(l0 == 0 && l1 == 2, "each range its own L1");
    nb = sst_names(before, 8);
    CHECK(nb == 2, "two L1 files");
    CHECK(efs_kv_put(kv, kc, 8, (const uint8_t *)"C", 1) == EFS_OK, "put C");
    CHECK(efs_kv_lsm_flush(kv) == EFS_OK, "flush one range");
    CHECK(efs_kv_lsm_compact(kv) == EFS_OK, "compact that range");
    na = sst_names(after, 8);
    for (i = 0; i < nb && i < 8; i++) {
        int j;

        for (j = 0; j < na && j < 8; j++)
            if (strcmp(before[i], after[j]) == 0)
                kept++;
    }
    CHECK(kept >= 1, "other range's L1 file was not rewritten");
    len = sizeof(buf);
    CHECK(efs_kv_get(kv, ka, 8, buf, &len) == EFS_OK && buf[0] == 'A', "A");
    len = sizeof(buf);
    CHECK(efs_kv_get(kv, kb, 8, buf, &len) == EFS_OK && buf[0] == 'B', "B");
    len = sizeof(buf);
    CHECK(efs_kv_get(kv, kc, 8, buf, &len) == EFS_OK && buf[0] == 'C', "C");
    efs_kv_lsm_close(kv);
}

/* Two flushed L0s share keys. The export heap must keep the newest
 * value, drop a newer tombstone, and omit the other raft group. */
static void test_view_export_overlap(void)
{
    struct efs_kv_lsm_cfg cfg;
    struct efs_kv *kv, *dst;
    struct efs_kv_lsm_view *view = NULL;
    uint8_t key[EFS_KV_KEY_MAX];
    uint8_t *blob = NULL;
    char path[600];
    uint32_t klen = 0, blen = 0;
    FILE *f;
    long sz;

    rmtree(g_dir);
    memset(&cfg, 0, sizeof(cfg));
    cfg.sync_mode = EFS_KV_LSM_NOSYNC;
    cfg.memtable_max = 4u * 1024u * 1024u;
    cfg.l0_max = 1000;
    kv = efs_kv_lsm_open(g_dir, &cfg);
    CHECK(kv != NULL, "view-export open");
    if (!kv)
        return;
    CHECK(put_ino(kv, 1, 1, "old") == EFS_OK, "old");
    CHECK(put_ino(kv, 1, 11, "keep") == EFS_OK, "keep");
    CHECK(put_ino(kv, 1, 21, "gone") == EFS_OK, "gone");
    CHECK(put_ino(kv, 2, 2, "even") == EFS_OK, "even");
    CHECK(efs_kv_lsm_flush(kv) == EFS_OK, "flush old");
    CHECK(put_ino(kv, 1, 1, "new") == EFS_OK, "new");
    CHECK(efs_kv_key_inode(1, 21, key, &klen) == EFS_OK, "del key");
    CHECK(efs_kv_del(kv, key, klen) == EFS_OK, "del");
    CHECK(put_ino(kv, 1, 31, "only-new") == EFS_OK, "only-new");
    CHECK(efs_kv_lsm_flush(kv) == EFS_OK, "flush new");
    CHECK(efs_kv_lsm_view_pin(kv, &view) == EFS_OK && view, "pin");
    snprintf(path, sizeof(path), "%s/snap.bin", g_dir);
    if (view) {
        CHECK(efs_kv_lsm_view_export(view, EFS_RAFT_GROUP_SHARD, path) == EFS_OK,
              "view export");
        efs_kv_lsm_view_unpin(view);
    }
    f = fopen(path, "rb");
    CHECK(f != NULL, "snap open");
    if (f) {
        fseek(f, 0, SEEK_END);
        sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        CHECK(sz > 4, "snap size");
        if (sz > 4) {
            blob = malloc((size_t)sz);
            CHECK(blob && fread(blob, 1, (size_t)sz, f) == (size_t)sz, "snap read");
            blen = (uint32_t)sz;
        }
        fclose(f);
    }
    dst = efs_kv_mem_create();
    CHECK(dst != NULL, "dst");
    if (dst && blob) {
        CHECK(efs_kv_group_import(dst, EFS_RAFT_GROUP_SHARD, blob, blen) == EFS_OK,
              "import view");
        CHECK(get_ino(dst, 1, 1, "new") == EFS_OK, "newest wins");
        CHECK(get_ino(dst, 1, 11, "keep") == EFS_OK, "older unique kept");
        CHECK(get_ino(dst, 1, 21, NULL) == EFS_ERR_NOT_FOUND, "tombstone dropped");
        CHECK(get_ino(dst, 1, 31, "only-new") == EFS_OK, "new unique kept");
        CHECK(get_ino(dst, 2, 2, NULL) == EFS_ERR_NOT_FOUND, "other group omitted");
        efs_kv_mem_free(dst);
    }
    free(blob);
    efs_kv_lsm_close(kv);
    rmtree(g_dir);
}

/* Deterministically pause a batch at the real durable-WAL/apply boundary.
 * A maintenance/background flush must preserve the appended tail. */
static void test_flush_with_unapplied_wal(void)
{
    pid_t pid; int status = 0;
    rmtree(g_dir);
    pid = fork();
    CHECK(pid >= 0, "fork WAL tail");
    if (pid < 0) return;
    if (pid == 0) {
        struct efs_kv *kv = open_store(EFS_KV_LSM_SYNC, 0);
        uint64_t seq = 0;
        struct efs_kv_item items[] = {
            { EFS_KV_PUT, (const uint8_t *)"same", 4,
              (const uint8_t *)"new", 3 },
            { EFS_KV_PUT, (const uint8_t *)"tail", 4,
              (const uint8_t *)"keep", 4 },
            { EFS_KV_DEL, (const uint8_t *)"deleted", 7, NULL, 0 }
        };
        if (!kv || put_s(kv,"same","old") || put_s(kv,"deleted","old")) _exit(1);
        struct kv_lsm *l = kv->ctx;
        if (kv_wal_append(l->wal,items,3,&seq) || seq != l->apply_next) _exit(2);
        if (efs_kv_lsm_flush(kv)) _exit(3);
        _exit(0); /* no destructor, exactly the crash window */
    }
    CHECK(waitpid(pid,&status,0)==pid && WIFEXITED(status) && WEXITSTATUS(status)==0,
          "durable append then crash after flush");
    struct efs_kv *kv = open_store(EFS_KV_LSM_SYNC,0);
    CHECK(kv != NULL,"reopen concurrent WAL tail");
    if (kv) {
        CHECK(get_is(kv,"same","new")==EFS_OK,"newer unapplied overwrite survives");
        CHECK(get_is(kv,"tail","keep")==EFS_OK,"unapplied insertion survives");
        CHECK(get_is(kv,"deleted","")==EFS_ERR_NOT_FOUND,"unapplied deletion survives");
        CHECK(put_s(kv,"after","progress")==EFS_OK,"apply ordering resumes after reopen");
        CHECK(efs_kv_lsm_flush(kv)==EFS_OK,"fully applied WAL can reset");
        efs_kv_lsm_close(kv);
    }
}

static int snapshot_is(struct efs_kv_lsm_snapshot *s, const char *key,
                         const char *value)
{
    uint8_t buf[256];
    uint32_t n=sizeof(buf);
    int rc=efs_kv_lsm_snapshot_get(s,(const uint8_t *)key,strlen(key),buf,&n);
    if (rc!=EFS_OK) return rc;
    return n==strlen(value) && !memcmp(buf,value,n) ? EFS_OK : EFS_ERR_PROTO;
}

struct snapshot_reader {
    struct efs_kv_lsm_snapshot *s;
    struct efs_kv *live;
    int bad;
};
static int snapshot_reenter(void *user, const uint8_t *key, uint32_t kl,
                            const uint8_t *value, uint32_t vl)
{
    struct snapshot_reader *r=user;
    (void)key;(void)kl;(void)value;(void)vl;
    /* Neither scan callbacks nor re-entrant reads may hold the live lock. */
    if (snapshot_is(r->s,"p/a","new") || put_s(r->live,"late","later"))
        r->bad++;
    return 0;
}
static void *snapshot_reader_thread(void *user)
{
    struct snapshot_reader *r=user;
    for (int i=0;i<100;i++) {
        struct acc a;
        memset(&a,0,sizeof(a));
        if (snapshot_is(r->s,"p/a","new") ||
            efs_kv_lsm_snapshot_scan(r->s,(const uint8_t *)"p/",2,NULL,0,acc_cb,&a) ||
            strcmp(a.buf,"p/a,p/c,")) r->bad++;
    }
    return NULL;
}
static void *snapshot_batch_writer(void *user)
{
    struct efs_kv *kv=user;
    for (int i=1;i<=100;i++) {
        char value[16];
        int n=snprintf(value,sizeof(value),"%d",i);
        struct efs_kv_item items[]={
            {EFS_KV_PUT,(const uint8_t *)"pair/a",6,(const uint8_t *)value,(uint32_t)n},
            {EFS_KV_PUT,(const uint8_t *)"pair/b",6,(const uint8_t *)value,(uint32_t)n}
        };
        if (efs_kv_batch(kv,items,2)) return (void *)1;
        usleep(1000);
    }
    return NULL;
}
static void test_snapshot_batch_cut(void)
{
    rmtree(g_dir);
    struct efs_kv *kv=open_store(EFS_KV_LSM_NOSYNC,0);
    CHECK(kv!=NULL,"open atomic snapshot test");
    if (!kv) return;
    CHECK(!put_s(kv,"pair/a","0") && !put_s(kv,"pair/b","0"),"initialize pair");
    pthread_t thread;
    int made=pthread_create(&thread,NULL,snapshot_batch_writer,kv)==0;
    CHECK(made,"start snapshot batch writer");
    for (int i=0;i<100 && made;i++) {
        struct efs_kv_lsm_snapshot *s=NULL;
        CHECK(!efs_kv_lsm_snapshot_pin(kv,1<<20,&s),"pin while batches apply");
        if (!s) break;
        uint8_t a[16],b[16];uint32_t an=sizeof(a),bn=sizeof(b);
        CHECK(!efs_kv_lsm_snapshot_get(s,(const uint8_t *)"pair/a",6,a,&an) &&
              !efs_kv_lsm_snapshot_get(s,(const uint8_t *)"pair/b",6,b,&bn) &&
              an==bn && !memcmp(a,b,an),"snapshot never observes half a KV batch");
        efs_kv_lsm_snapshot_release(s);
        usleep(1000);
    }
    if (made) {
        void *result=NULL;
        CHECK(!pthread_join(thread,&result) && !result,"join snapshot batch writer");
    }
    efs_kv_lsm_close(kv);
}

static void test_snapshot_scan_bounds(void)
{
    rmtree(g_dir);struct efs_kv *kv=open_store(EFS_KV_LSM_NOSYNC,0);
    CHECK(kv!=NULL,"open bounded scan source");if(!kv)return;
    CHECK(!put_s(kv,"p/a","value") && !efs_kv_lsm_flush(kv) && !efs_kv_lsm_compact(kv) && !efs_kv_lsm_quiesce(kv),"persist bounded source");
    struct kv_lsm *live=kv->ctx,mock=*live;
    struct kv_seg *seg=live->n_l0?live->l0[0].seg:live->l1[0].seg;
    memset(&mock.mu,0,sizeof(mock.mu));pthread_mutex_init(&mock.mu,NULL);
    mock.l0=calloc(KV_LSM_SNAPSHOT_SCAN_SEGS+1,sizeof(*mock.l0));mock.n_l1=0;mock.l1=NULL;
    CHECK(mock.l0!=NULL,"allocate white-box fan-in boundary");
    if(mock.l0){
        for(unsigned i=0;i<KV_LSM_SNAPSHOT_SCAN_SEGS+1;i++)mock.l0[i].seg=seg;
        struct efs_kv input={kv->ops,&mock};struct efs_kv_lsm_snapshot *source=NULL;struct acc a={0};
        mock.n_l0=KV_LSM_SNAPSHOT_SCAN_SEGS;
        CHECK(!efs_kv_lsm_snapshot_pin(&input,1<<20,&source),"pin exact fan-in boundary");
        if(source){CHECK(!efs_kv_lsm_snapshot_scan(source,(const uint8_t *)"p/",2,NULL,0,acc_cb,&a),"exact fan-in admits source");efs_kv_lsm_snapshot_release(source);source=NULL;}
        mock.n_l0=KV_LSM_SNAPSHOT_SCAN_SEGS+1;
        CHECK(!efs_kv_lsm_snapshot_pin(&input,1<<20,&source),"pin above scan fan-in");
        if(source){
            memset(&a,0,sizeof(a));
            CHECK(efs_kv_lsm_snapshot_scan(source,(const uint8_t *)"p/",2,NULL,0,acc_cb,&a)==EFS_ERR_BUSY && !a.n,"over fan-in refuses before output");
            CHECK(!efs_kv_lsm_snapshot_scan(source,(const uint8_t *)"z/",2,NULL,0,acc_cb,&a) && !a.n,"irrelevant sources do not consume scan fan-in");
            efs_kv_lsm_snapshot_release(source);
        }
        free(mock.l0);
    }
    pthread_mutex_destroy(&mock.mu);
    uint32_t size=KV_LSM_SNAPSHOT_BLOCK_MAX+1;uint8_t *large=malloc(size),*got=malloc(size);
    CHECK(large && got,"allocate oversized-block test input");
    if(large && got){
        memset(large,0x5a,size);
        CHECK(!efs_kv_put(kv,(const uint8_t *)"large",5,large,size) && !efs_kv_lsm_flush(kv),"persist large authoritative value");
        struct efs_kv_lsm_snapshot *source=NULL;struct acc a={0};
        CHECK(!efs_kv_lsm_snapshot_pin(kv,1<<20,&source),"pin large-block source");
        if(source){CHECK(efs_kv_lsm_snapshot_scan(source,(const uint8_t *)"large",5,NULL,0,acc_cb,&a)==EFS_ERR_BUSY && !a.n,"oversized private block refuses before output");
            CHECK(!efs_kv_lsm_snapshot_scan(source,(const uint8_t *)"largf",5,NULL,0,acc_cb,&a) && !a.n,"cached tail excludes irrelevant oversized block without reading it");
            efs_kv_lsm_snapshot_release(source);}
        uint32_t capacity=size;
        CHECK(!efs_kv_get(kv,(const uint8_t *)"large",5,got,&capacity) && capacity==size && !memcmp(got,large,size),"private scan refusal leaves authoritative reads valid");
    }
    free(large);free(got);efs_kv_lsm_close(kv);
}

static void test_full_snapshot(void)
{
    struct efs_kv_lsm_snapshot *s=NULL,*failed=NULL;
    struct efs_kv *kv;
    struct acc a;
    uint8_t buf[4];
    uint32_t n=1;
    rmtree(g_dir);
    kv=open_store(EFS_KV_LSM_NOSYNC,0);
    CHECK(kv!=NULL,"open full snapshot");
    if (!kv) return;
    CHECK(put_s(kv,"p/a","old")==EFS_OK,"base a");
    CHECK(put_s(kv,"p/b","base")==EFS_OK,"base b");
    CHECK(put_s(kv,"p/c","old")==EFS_OK,"base c");
    CHECK(efs_kv_lsm_flush(kv)==EFS_OK,"base flush");
    CHECK(efs_kv_lsm_compact(kv)==EFS_OK,"base compact");
    CHECK(put_s(kv,"p/a","new")==EFS_OK,"unflushed override");
    CHECK(efs_kv_del(kv,(const uint8_t *)"p/b",3)==EFS_OK,"unflushed delete");
    CHECK(put_s(kv,"p/c","")==EFS_OK,"unflushed empty value");
    CHECK(efs_kv_lsm_snapshot_pin(kv,1,&failed)==EFS_ERR_BUSY && !failed,
          "snapshot budget is explicit");
    struct kv_lsm *budget_store=kv->ctx;
    pthread_mutex_lock(&budget_store->mu);
    struct kv_lsm overflow_store=*budget_store;
    pthread_mutex_unlock(&budget_store->mu);
    /* Never expose deliberately invalid counts to the live compactor. */
    memset(&overflow_store.mu,0,sizeof(overflow_store.mu));pthread_mutex_init(&overflow_store.mu,NULL);
    overflow_store.n_l0=UINT32_MAX;overflow_store.n_l1=1;
    struct efs_kv overflow_kv={kv->ops,&overflow_store};
    CHECK(efs_kv_lsm_snapshot_pin(&overflow_kv,UINT64_MAX,&failed)==EFS_ERR_BUSY && !failed,
          "overflowed segment count refuses before touching segment pointers");
    pthread_mutex_destroy(&overflow_store.mu);
    CHECK(efs_kv_lsm_snapshot_pin(kv,1<<20,&s)==EFS_OK,"pin unflushed state");
    if (!s) { efs_kv_lsm_close(kv);return; }
    uint64_t snapshot_bytes=efs_kv_lsm_snapshot_bytes(s);
    CHECK(snapshot_bytes<=1<<20 && snapshot_bytes>budget_store->mt.bytes,
          "reported source bytes include copied values, object and segment lists");
    CHECK(efs_kv_lsm_snapshot_pin(kv,snapshot_bytes-1,&failed)==EFS_ERR_BUSY && !failed,
          "one byte below retained source requirement refuses");
    CHECK(!efs_kv_lsm_snapshot_pin(kv,snapshot_bytes,&failed),"exact source budget succeeds");
    if(failed){CHECK(efs_kv_lsm_snapshot_bytes(failed)==snapshot_bytes,"exact charged snapshot size");efs_kv_lsm_snapshot_release(failed);failed=NULL;}
    CHECK(snapshot_is(s,"p/a","new")==EFS_OK,"snapshot includes memtable override");
    CHECK(snapshot_is(s,"p/b","")==EFS_ERR_NOT_FOUND,"snapshot applies tombstone");
    CHECK(snapshot_is(s,"p/c","")==EFS_OK,"snapshot preserves empty value");
    CHECK(snapshot_is(s,"p","")==EFS_ERR_NOT_FOUND,"prefix is not exact key");
    CHECK(efs_kv_lsm_snapshot_get(s,(const uint8_t *)"p/a",3,buf,&n)==EFS_ERR_INVAL && n==3,
          "snapshot get reports required size");
    CHECK(put_s(kv,"p/a","later")==EFS_OK,"live overwrite");
    CHECK(put_s(kv,"p/b","restored")==EFS_OK,"live recreation");
    CHECK(efs_kv_del(kv,(const uint8_t *)"p/c",3)==EFS_OK,"live delete");
    CHECK(efs_kv_lsm_flush(kv)==EFS_OK,"live flush");
    CHECK(efs_kv_lsm_compact(kv)==EFS_OK,"live compact");
    memset(&a,0,sizeof(a));
    CHECK(efs_kv_lsm_snapshot_scan(s,(const uint8_t *)"p/",2,(const uint8_t *)"p/b",3,acc_cb,&a)==EFS_OK,
          "snapshot paged seek");
    CHECK(!strcmp(a.buf,"p/c,"),"snapshot pagination preserves deletion");
    struct snapshot_reader re={s,kv,0};
    CHECK(efs_kv_lsm_snapshot_scan(s,(const uint8_t *)"p/",2,NULL,0,snapshot_reenter,&re)==EFS_OK && !re.bad,
          "snapshot callback may read itself and write live KV");
    pthread_t threads[4];
    struct snapshot_reader readers[4];
    int made=0;
    for (int i=0;i<4;i++) {
        readers[i]=(struct snapshot_reader){s,kv,0};
        if (pthread_create(&threads[i],NULL,snapshot_reader_thread,&readers[i])) break;
        made++;
    }
    CHECK(made==4,"start concurrent snapshot readers");
    for (int i=0;i<made;i++) {
        CHECK(!pthread_join(threads[i],NULL) && !readers[i].bad,"concurrent snapshot reader stable");
    }
    CHECK(snapshot_is(s,"late","")==EFS_ERR_NOT_FOUND,"snapshot excludes callback mutations");
    efs_kv_lsm_snapshot_release(s);
    /* An IO-poisoned engine must not present partially applied state. */
    struct kv_lsm *l=kv->ctx;
    pthread_mutex_lock(&l->mu);l->io_failed=1;pthread_mutex_unlock(&l->mu);
    CHECK(efs_kv_lsm_snapshot_pin(kv,1<<20,&failed)==EFS_ERR_IO && !failed,"poisoned snapshot refused");
    pthread_mutex_lock(&l->mu);l->io_failed=0;pthread_mutex_unlock(&l->mu);
    efs_kv_lsm_close(kv);
}

static int durable_hints(struct efs_kv *kv,uint64_t id[2],uint64_t revisions[4096])
{
    struct efs_kv_lsm_snapshot *s=NULL;int rc=efs_kv_lsm_snapshot_pin(kv,1<<20,&s);
    if(!rc)rc=efs_kv_lsm_snapshot_query_revisions(s,id,revisions);
    efs_kv_lsm_snapshot_release(s);return rc;
}
static void test_durable_query_hints(void)
{
    rmtree(g_dir);struct efs_kv *kv=open_store(EFS_KV_LSM_SYNC,0);
    CHECK(kv,"durable hints open");if(!kv)return;
    uint8_t key[]={0,17,EFS_KV_KIND_INODE,1};uint64_t id[2],saved[2],r[4096];
    CHECK(!durable_hints(kv,saved,r) && !r[17],"new durable lineage");
    CHECK(!efs_kv_put(kv,key,sizeof(key),(const uint8_t *)"a",1),"first durable metadata");
    CHECK(!efs_kv_lsm_flush(kv) && !efs_kv_lsm_quiesce(kv),"flush durable hints with source");
    CHECK(!durable_hints(kv,id,r) && r[17]==1,"flushed counter");
    efs_kv_lsm_query_tracking(kv,0);
    CHECK(!efs_kv_put(kv,key,sizeof(key),(const uint8_t *)"b",1),"disabled metadata writes remain tracked durably");
    CHECK(!durable_hints(kv,id,r) && r[17]==2,"disabled counter advances");
    efs_kv_lsm_close(kv);kv=open_store(EFS_KV_LSM_SYNC,0);CHECK(kv,"WAL tail reopen");if(!kv)return;
    CHECK(!durable_hints(kv,id,r) && !memcmp(id,saved,sizeof(id)) && r[17]==2,"same lineage recovers flushed plus WAL counters");
    CHECK(!efs_kv_lsm_flush(kv) && !efs_kv_lsm_quiesce(kv),"persist recovered counters");
    efs_kv_lsm_close(kv);kv=open_store(EFS_KV_LSM_SYNC,0);CHECK(kv,"flushed reopen");if(!kv)return;
    CHECK(!durable_hints(kv,id,r) && !memcmp(id,saved,sizeof(id)) && r[17]==2,"flushed counters do not reset or double");
    char blocked[sizeof(g_dir)+40];snprintf(blocked,sizeof(blocked),"%s/QUERY-REVISIONS.new",g_dir);
    CHECK(!mkdir(blocked,0700),"block optional checkpoint write");
    CHECK(!efs_kv_put(kv,key,sizeof(key),(const uint8_t *)"c",1) && !efs_kv_lsm_flush(kv) && !efs_kv_lsm_quiesce(kv),"derived checkpoint failure cannot fail durable authority writes");
    efs_kv_lsm_close(kv);kv=open_store(EFS_KV_LSM_SYNC,0);CHECK(kv,"stale sidecar reopen");if(!kv)return;
    CHECK(!durable_hints(kv,id,r) && memcmp(id,saved,sizeof(id)),"manifest mismatch refuses stale lineage");
    CHECK(!rmdir(blocked),"remove checkpoint fault");
    efs_kv_lsm_close(kv);
    char path[sizeof(g_dir)+40];snprintf(path,sizeof(path),"%s/QUERY-REVISIONS",g_dir);
    int fd=open(path,O_RDWR);CHECK(fd>=0,"open sidecar for corruption");if(fd>=0){uint8_t bad=0;CHECK(pwrite(fd,&bad,1,0)==1,"corrupt sidecar magic");close(fd);}
    kv=open_store(EFS_KV_LSM_SYNC,0);CHECK(kv,"corrupt derived hints do not poison authority");if(!kv)return;
    CHECK(!durable_hints(kv,id,r) && memcmp(id,saved,sizeof(id)),"corrupt hints create different lineage");
    uint8_t value[2];uint32_t length=sizeof(value);
    CHECK(!efs_kv_get(kv,key,sizeof(key),value,&length) && length==1 && value[0]=='c',"authority survives hint corruption");
    memcpy(saved,id,sizeof(id));efs_kv_lsm_close(kv);
    CHECK(!unlink(path),"remove derived sidecar");kv=open_store(EFS_KV_LSM_SYNC,0);CHECK(kv,"missing derived hints reopen");
    if(kv){CHECK(!durable_hints(kv,id,r) && memcmp(id,saved,sizeof(id)),"missing sidecar forces a fresh lineage");efs_kv_lsm_close(kv);}
}

static void test_metadata_dirty_hints(void)
{
    rmtree(g_dir);
    struct efs_kv *kv=open_store(EFS_KV_LSM_NOSYNC,0);
    CHECK(kv!=NULL,"open dirty hints");
    if (!kv) return;
    uint64_t before[4096],after[4096],pinned[4096];
    uint8_t inode[]={0,3,EFS_KV_KIND_INODE,1};
    uint8_t dent[]={0,3,EFS_KV_KIND_DENTRY,2};
    uint8_t intent[]={0,4,EFS_KV_KIND_INTENT,3};
    uint8_t chunk[]={0,3,EFS_KV_KIND_CHUNK,4};
    struct efs_kv_item items[]={
        {EFS_KV_PUT,inode,sizeof(inode),(const uint8_t *)"i",1},
        {EFS_KV_PUT,dent,sizeof(dent),(const uint8_t *)"d",1},
        {EFS_KV_PUT,intent,sizeof(intent),(const uint8_t *)"t",1},
        {EFS_KV_PUT,chunk,sizeof(chunk),(const uint8_t *)"c",1}
    };
    CHECK(!efs_kv_lsm_metadata_revisions(kv,before,4096),"initial hints");
    CHECK(!before[3] && !before[4],"hints start cold");
    CHECK(!efs_kv_batch(kv,items,4),"dirty metadata and chunk batch");
    struct efs_kv_lsm_snapshot *s=NULL;
    CHECK(!efs_kv_lsm_snapshot_pin(kv,1<<20,&s),"pin dirty source");
    if (!s) { efs_kv_lsm_close(kv);return; }
    CHECK(!efs_kv_lsm_snapshot_revisions(s,pinned,4096) && pinned[3]==2 && pinned[4]==1,
          "snapshot revisions include inode/dentry/intent, exclude payload maps");
    CHECK(!efs_kv_del(kv,dent,sizeof(dent)),"dirty delete");
    CHECK(!efs_kv_lsm_metadata_revisions(kv,after,4096) && after[3]==3 && after[4]==1,
          "delete advances only its shard");
    CHECK(!efs_kv_lsm_snapshot_revisions(s,before,4096) && !memcmp(before,pinned,sizeof(before)),
          "snapshot revision does not advance with live changes");
    struct kv_lsm *l=kv->ctx;
    pthread_mutex_lock(&l->mu);l->fail_next_batch=1;pthread_mutex_unlock(&l->mu);
    CHECK(efs_kv_batch(kv,items,4)==EFS_ERR_IO,"failed batch");
    CHECK(!efs_kv_lsm_metadata_revisions(kv,before,4096) && !memcmp(before,after,sizeof(before)),
          "rejected batch cannot advance dirty hints");
    pthread_mutex_lock(&l->mu);l->metadata_revision[3]=UINT64_MAX-1;pthread_mutex_unlock(&l->mu);
    CHECK(!efs_kv_put(kv,inode,sizeof(inode),(const uint8_t *)"i",1),"write exhausting revision");
    CHECK(efs_kv_lsm_metadata_revisions(kv,before,4096)==EFS_ERR_OVERFLOW,"live saturated hints refuse comparisons");
    CHECK(!efs_kv_put(kv,inode,sizeof(inode),(const uint8_t *)"i",1),"write after hint saturation");
    CHECK(efs_kv_lsm_metadata_revisions(kv,before,4096)==EFS_ERR_OVERFLOW,"dirty hints never wrap to zero");
    struct efs_kv_lsm_snapshot *saturated=NULL;
    CHECK(!efs_kv_lsm_snapshot_pin(kv,1<<20,&saturated),"saturated hint does not poison authoritative reads");
    if(saturated){
        CHECK(efs_kv_lsm_snapshot_revisions(saturated,before,4096)==EFS_ERR_OVERFLOW,"snapshot saturated hints refuse comparisons");
        efs_kv_lsm_snapshot_release(saturated);
    }
    CHECK(!efs_kv_lsm_snapshot_revisions(s,before,4096) && !memcmp(before,pinned,sizeof(before)),"retained finite snapshot remains valid after live saturation");
    efs_kv_lsm_snapshot_release(s);
    efs_kv_lsm_close(kv);
    kv=open_store(EFS_KV_LSM_NOSYNC,0);
    CHECK(kv!=NULL,"reopen dirty hints");
    if (kv) {
        CHECK(!efs_kv_lsm_metadata_revisions(kv,before,4096) && !before[3] && !before[4],
              "hints reset on reopen and require initial index rebuild");
        CHECK(get_is(kv,(const char *)"absent","")==EFS_ERR_NOT_FOUND,"reopened reads work");
        efs_kv_lsm_close(kv);
    }
}

static void test_pressure_observation(void)
{
    struct efs_kv_lsm_pressure p;
    struct efs_kv *kv, *mem;
    struct kv_lsm *l;
    uint64_t saved_bytes;

    rmtree(g_dir);
    kv = open_store(EFS_KV_LSM_SYNC, 1024);
    CHECK(kv != NULL, "pressure store opens");
    if (!kv) return;
    CHECK(efs_kv_lsm_pressure(kv, &p) == EFS_OK && !p.soft && !p.hard,
          "empty engine is not pressured");
    CHECK(p.memtable_target == 1024 && p.l0_hard_bytes == (1ull << 30) &&
          p.l0_soft_bytes == (768ull << 20), "explicit unchanged cap and early watermark");
    l = kv->ctx;
    pthread_mutex_lock(&l->mu);
    saved_bytes = l->mt.bytes;
    l->mt.bytes = 2048;
    pthread_mutex_unlock(&l->mu);
    CHECK(efs_kv_lsm_pressure(kv, &p) == EFS_OK && p.soft && p.hard,
          "deferred oversized memtable reports pressure");
    pthread_mutex_lock(&l->mu);
    l->mt.bytes = saved_bytes;
    l->io_failed = 1;
    pthread_mutex_unlock(&l->mu);
    CHECK(efs_kv_lsm_pressure(kv, &p) == EFS_OK && p.io_failed && p.hard,
          "poisoned persistence cannot report healthy capacity");
    pthread_mutex_lock(&l->mu);
    l->io_failed = 0;
    pthread_mutex_unlock(&l->mu);
    CHECK(efs_kv_lsm_pressure(kv, &p) == EFS_OK && !p.hard,
          "observation follows recovered state");
    CHECK(put_s(kv, "pressure", "committed") == EFS_OK,
          "observation does not alter apply semantics");
    efs_kv_lsm_close(kv);
    kv = open_store(EFS_KV_LSM_SYNC, 1024);
    CHECK(kv && get_is(kv, "pressure", "committed") == EFS_OK,
          "observed engine retains exact durable bytes");
    if (kv) efs_kv_lsm_close(kv);
    mem = efs_kv_mem_create();
    CHECK(efs_kv_lsm_pressure(mem, &p) == EFS_ERR_INVAL,
          "non-LSM engine is unavailable rather than zero pressure");
    efs_kv_mem_free(mem);
    CHECK(efs_kv_lsm_pressure(NULL, &p) == EFS_ERR_INVAL &&
          efs_kv_lsm_pressure(NULL, NULL) == EFS_ERR_INVAL, "invalid pressure inputs");
}


/* Recovery relies on a cursor sharing the exact atomic WAL record, not a
 * later file sync. Test staged reads/scans, exclusion, abort and real exits. */
struct apply_peer {struct efs_kv *kv;int saw_old,entered,done;};
static void *apply_peer(void *p)
{
 struct apply_peer *a=p;
 a->saw_old=get_is(a->kv,"a","old")==EFS_OK;
 __atomic_store_n(&a->entered,1,__ATOMIC_RELEASE);
 CHECK(put_s(a->kv,"peer","after")==EFS_OK,"writer proceeds after atomic scope");
 __atomic_store_n(&a->done,1,__ATOMIC_RELEASE);
 return NULL;
}
static void test_apply_scope(void)
{
 rmtree(g_dir);
 struct efs_kv *kv=open_store(EFS_KV_LSM_SYNC,0);
 CHECK(kv!=NULL,"open apply-scope store");if(!kv)return;
 CHECK(put_s(kv,"a","old")==EFS_OK && put_s(kv,"b","deleted")==EFS_OK,"scope baseline");
 CHECK(efs_kv_lsm_flush(kv)==EFS_OK,"scope reads cross disk and overlay");
 struct efs_kv_item cursor={EFS_KV_PUT,(const uint8_t *)"cursor",6,(const uint8_t *)"1",1};
 CHECK(efs_kv_lsm_apply_begin(kv)==EFS_OK,"begin bounded scope");
 CHECK(efs_kv_lsm_apply_begin(kv)==EFS_ERR_BUSY,"nested scope refused");
 CHECK(put_s(kv,"a","new")==EFS_OK && put_s(kv,"c","added")==EFS_OK,"stage overwrites/inserts");
 CHECK(efs_kv_del(kv,(const uint8_t *)"b",1)==EFS_OK,"stage deletion");
 CHECK(get_is(kv,"a","new")==EFS_OK && get_is(kv,"b","")==EFS_ERR_NOT_FOUND,"own staged view");
 struct acc scan={0};CHECK(efs_kv_scan(kv,acc_cb,&scan)==EFS_OK,"overlay scan");
 CHECK(!strcmp(scan.buf,"a,c,"),"scan deduplicates and hides tombstones");
 memset(&scan,0,sizeof(scan));
 CHECK(efs_kv_scan_from(kv,NULL,0,(const uint8_t *)"b",1,acc_cb,&scan)==EFS_OK && !strcmp(scan.buf,"c,"),"staged range scan seeks all sources");
 struct apply_peer peer={.kv=kv};pthread_t t;
 CHECK(!pthread_create(&t,NULL,apply_peer,&peer),"start excluded writer");
 while(!__atomic_load_n(&peer.entered,__ATOMIC_ACQUIRE))usleep(1000);
 CHECK(peer.saw_old && !__atomic_load_n(&peer.done,__ATOMIC_ACQUIRE),"other reader sees old state; writer excluded");
 CHECK(efs_kv_lsm_apply_end(kv,&cursor)==EFS_OK,"effects/cursor atomically committed");
 pthread_join(t,NULL);
 CHECK(get_is(kv,"cursor","1")==EFS_OK && get_is(kv,"a","new")==EFS_OK,"committed view");
 /* A staged DEL with garbage val/vlen (scan-callback items) must stage
  * and commit, not poison the scope with INVAL (LANE_FENCE wedge). */
 CHECK(put_s(kv,"g","gone")==EFS_OK,"garbage-del baseline");
 CHECK(efs_kv_lsm_apply_begin(kv)==EFS_OK,"begin garbage-del scope");
 {
  struct efs_kv_item del={EFS_KV_DEL,(const uint8_t *)"g",1,(const uint8_t *)0xdeadbeef,0xffffffffu};
  CHECK(efs_kv_batch(kv,&del,1)==EFS_OK,"staged del ignores val/vlen");
 }
 CHECK(efs_kv_lsm_apply_end(kv,&cursor)==EFS_OK,"garbage-vlen del commits");
 CHECK(get_is(kv,"g","")==EFS_ERR_NOT_FOUND,"staged garbage-vlen del applied");
 CHECK(efs_kv_lsm_apply_begin(kv)==EFS_OK && put_s(kv,"a","abort")==EFS_OK,"stage aborted change");
 CHECK(efs_kv_lsm_apply_end(kv,NULL)==EFS_OK && get_is(kv,"a","new")==EFS_OK,"abort exposes no changes");
 CHECK(efs_kv_lsm_apply_begin(kv)==EFS_OK && put_s(kv,"a","failed")==EFS_OK,"stage rejected commit");
 ((struct kv_lsm *)kv->ctx)->fail_next_batch=1;
 cursor.val=(const uint8_t *)"2";
 CHECK(efs_kv_lsm_apply_end(kv,&cursor)==EFS_ERR_IO,"commit failure audible");
 CHECK(get_is(kv,"cursor","1")==EFS_OK && get_is(kv,"a","new")==EFS_OK,"failed commit preserves cursor and effects");
 CHECK(efs_kv_lsm_apply_begin(kv)==EFS_OK,"begin bounded key refusal");
 for(unsigned i=0;i<32768;i++) {
  char key[32];snprintf(key,sizeof(key),"bound-%08u",i);
  CHECK(put_s(kv,key,"x")==EFS_OK,"key within stage bound");
 }
 CHECK(put_s(kv,"over-bound","x")==EFS_ERR_NOMEM,"key bound refuses excess");
 CHECK(efs_kv_lsm_apply_end(kv,&cursor)==EFS_ERR_NOMEM,"overflow cannot commit a partial entry/cursor");
 CHECK(get_is(kv,"bound-00000000","")==EFS_ERR_NOT_FOUND && get_is(kv,"cursor","1")==EFS_OK,"overflow leaves authoritative state unchanged");
 CHECK(efs_kv_lsm_apply_begin(kv)==EFS_OK,"begin bounded byte refusal");
 uint8_t *large=calloc(1,16u<<20);CHECK(large!=NULL,"allocate one shared limit probe");
 if(large) {
  for(unsigned i=0;i<4;i++) {
   char key[16];snprintf(key,sizeof(key),"large-%u",i);
   int rc=efs_kv_put(kv,(uint8_t *)key,strlen(key),large,16u<<20);
   CHECK(rc==(i<3?EFS_OK:EFS_ERR_NOMEM),"payload bound includes metadata overhead");
  }
  free(large);
  CHECK(efs_kv_lsm_apply_end(kv,&cursor)==EFS_ERR_NOMEM,"byte overflow aborts whole entry");
 } else CHECK(efs_kv_lsm_apply_end(kv,NULL)==EFS_OK,"abort unavailable limit probe");
 CHECK(get_is(kv,"large-0","")==EFS_ERR_NOT_FOUND,"oversized entry leaves no bytes");
 efs_kv_lsm_close(kv);
 for(int commit=0;commit<=1;commit++) {
  pid_t child=fork();CHECK(child>=0,"fork actual scope crash");
  if(!child) {
   struct efs_kv *c=open_store(EFS_KV_LSM_SYNC,0);
   if(!c || efs_kv_lsm_apply_begin(c)!=EFS_OK || put_s(c,"a","crash")!=EFS_OK)_exit(2);
   if(commit && efs_kv_lsm_apply_end(c,&cursor)!=EFS_OK)_exit(3);
   _exit(0); /* no close/flush/destructor can complete an uncommitted scope */
  }
  int status=0;waitpid(child,&status,0);CHECK(WIFEXITED(status)&&!WEXITSTATUS(status),"scope child succeeded");
  kv=open_store(EFS_KV_LSM_SYNC,0);CHECK(kv!=NULL,"cold scope reopen");
  CHECK(get_is(kv,"a",commit?"crash":"new")==EFS_OK,"cold effects exactly match commit");
  CHECK(get_is(kv,"cursor",commit?"2":"1")==EFS_OK,"cold cursor exactly matches effects");
  efs_kv_lsm_close(kv);
 }
 /* A torn final WAL record must expose neither its effects nor cursor. */
 kv=open_store(EFS_KV_LSM_SYNC,0);
 CHECK(put_s(kv,"a","base")==EFS_OK && put_s(kv,"cursor","3")==EFS_OK,"torn record baseline");
 efs_kv_lsm_close(kv);
 pid_t child=fork();CHECK(child>=0,"fork torn atomic record");
 if(!child) {
  struct efs_kv *c=open_store(EFS_KV_LSM_SYNC,0);
  cursor.val=(const uint8_t *)"4";
  if(!c || efs_kv_lsm_apply_begin(c)!=EFS_OK || put_s(c,"a","torn")!=EFS_OK ||
     efs_kv_lsm_apply_end(c,&cursor)!=EFS_OK)_exit(4);
  _exit(0);
 }
 int status=0;waitpid(child,&status,0);CHECK(WIFEXITED(status)&&!WEXITSTATUS(status),"torn record child committed");
 char wal[1024];
 snprintf(wal,sizeof(wal),"%s/wal",g_dir);
 /* Linux reserves a zero-filled tail; tear the logical record, not allocation. */
 FILE *f=fopen(wal,"rb");uint64_t end=0;uint8_t header[8];
 CHECK(f!=NULL,"read owned WAL record lengths");
 if(f) {
  while(fread(header,1,sizeof(header),f)==sizeof(header)) {
   uint32_t length=(uint32_t)header[4]|(uint32_t)header[5]<<8|
                   (uint32_t)header[6]<<16|(uint32_t)header[7]<<24;
   if(!length)break;
   CHECK(length>=4 && length<=KV_LSM_REC_MAX,"bounded valid fixture record");
   if(length<4 || length>KV_LSM_REC_MAX)break;
   end+=8+length;if(fseeko(f,length,SEEK_CUR))break;
  }
  fclose(f);
 }
 CHECK(end>8 && !truncate(wal,(off_t)end-1),"tear final actual record before reserved tail");
 kv=open_store(EFS_KV_LSM_SYNC,0);
 CHECK(kv && get_is(kv,"a","base")==EFS_OK && get_is(kv,"cursor","3")==EFS_OK,"torn atomic record exposes neither effects nor frontier");
 if(kv)efs_kv_lsm_close(kv);
 rmtree(g_dir);
}

/* Automatic flush deferral preserves nested holds, durable WAL order and cold recovery. */
static void test_deferred_held_flush(void)
{
    rmtree(g_dir);
    struct efs_kv *kv=open_store(EFS_KV_LSM_SYNC,256);
    CHECK(kv!=NULL,"open ordinary auto flush");if(!kv)return;
    char value[129];memset(value,'x',128);value[128]=0;
    for(int i=0;i<8;i++){char k[16];snprintf(k,sizeof(k),"normal%d",i);CHECK(!put_s(kv,k,value),"normal put");}
    struct efs_kv_lsm_stats shape={0};CHECK(!efs_kv_lsm_stats(kv,&shape) && shape.n_l0+shape.n_l1>0,"ordinary policy still auto flushes");
    efs_kv_lsm_close(kv);rmtree(g_dir);
    kv=open_store(EFS_KV_LSM_SYNC,256);CHECK(kv!=NULL,"open deferred flush");if(!kv)return;
    CHECK(!efs_kv_lsm_defer_held_flush(kv,1),"startup opt in");
    CHECK(!efs_kv_lsm_sync_hold(kv) && !efs_kv_lsm_sync_hold(kv),"nested deferred holds");
    CHECK(efs_kv_lsm_defer_held_flush(kv,0)==EFS_ERR_BUSY,"cannot change active policy");
    uint64_t detached=99;
    CHECK(efs_kv_lsm_sync_detach(kv,&detached)==EFS_ERR_BUSY && detached==0,
          "deferred flush hold cannot be detached without flush accounting");
    for(int i=0;i<32;i++){char k[16];snprintf(k,sizeof(k),"held%d",i);CHECK(!put_s(kv,k,value),"deferred visible ordered put");}
    CHECK(!get_is(kv,"held0",value),"held effects remain readable");
    CHECK(!efs_kv_lsm_stats(kv,&shape) && shape.mt_bytes>=256 && !shape.n_l0 && !shape.n_l1,"no inline automatic flush during hold");
    CHECK(!efs_kv_lsm_sync_release(kv),"inner release");
    CHECK(efs_kv_lsm_sync_detach(kv,&detached)==EFS_ERR_BUSY && detached==0,
          "single deferred flush hold also refuses detachment");
    CHECK(!efs_kv_lsm_stats(kv,&shape) && shape.mt_bytes>=256 && !shape.n_l0 && !shape.n_l1,"nested release cannot flush before outer WAL barrier");
    CHECK(!efs_kv_lsm_sync_release(kv),"outer barrier and same automatic flush");
    CHECK(!efs_kv_lsm_stats(kv,&shape) && !shape.mt_bytes && shape.n_l0+shape.n_l1>0,"outer release completed deferred flush");
    efs_kv_lsm_close(kv);kv=open_store(EFS_KV_LSM_SYNC,256);CHECK(kv!=NULL,"cold deferred reopen");
    if(kv){for(int i=0;i<32;i++){char k[16];snprintf(k,sizeof(k),"held%d",i);CHECK(!get_is(kv,k,value),"cold deferred effects");}efs_kv_lsm_close(kv);}
    /* Crash after release with a newer WAL record not yet in the memtable.
     * The deferred flush must preserve that tail rather than reset it. */
    rmtree(g_dir);pid_t pid=fork();CHECK(pid>=0,"fork deferred WAL tail");if(pid<0)return;
    if(!pid){
        kv=open_store(EFS_KV_LSM_SYNC,256);if(!kv || efs_kv_lsm_defer_held_flush(kv,1) || efs_kv_lsm_sync_hold(kv))_exit(1);
        for(int i=0;i<16;i++){char k[16];snprintf(k,sizeof(k),"cold%d",i);if(put_s(kv,k,value))_exit(2);}
        struct kv_lsm *l=kv->ctx;uint64_t seq=0;
        struct efs_kv_item item={EFS_KV_PUT,(const uint8_t *)"cold0",5,(const uint8_t *)"new-tail",8};
        if(kv_wal_append(l->wal,&item,1,&seq) || seq!=l->apply_next || efs_kv_lsm_sync_release(kv))_exit(3);
        _exit(0); /* No clean shutdown to make this pass. */
    }
    int status=0;CHECK(waitpid(pid,&status,0)==pid && WIFEXITED(status) && !WEXITSTATUS(status),"actual crash after deferred release");
    kv=open_store(EFS_KV_LSM_SYNC,256);CHECK(kv!=NULL,"cold preserved tail");
    if(kv){CHECK(!get_is(kv,"cold0","new-tail"),"unapplied newer tail survives deferred flush");CHECK(!get_is(kv,"cold1",value),"older flushed key also survives");CHECK(!put_s(kv,"after","progress"),"cold apply ordering resumes");efs_kv_lsm_close(kv);}
    /* A real SST-open failure after the WAL barrier must fail release,
     * reject further mutation and retain the durable effects for cold replay. */
    if(geteuid()!=0){
        rmtree(g_dir);kv=open_store(EFS_KV_LSM_SYNC,256);CHECK(kv!=NULL,"open deferred failure");if(!kv)return;
        CHECK(!efs_kv_lsm_defer_held_flush(kv,1) && !efs_kv_lsm_sync_hold(kv),"failure held setup");
        for(int i=0;i<16;i++){char k[16];snprintf(k,sizeof(k),"failed%d",i);CHECK(!put_s(kv,k,value),"WAL effects before flush failure");}
        CHECK(!chmod(g_dir,0500),"deny new SST publication");
        CHECK(efs_kv_lsm_sync_release(kv)==EFS_ERR_IO,"failed flush cannot report successful release");
        CHECK(put_s(kv,"rejected","mutation")==EFS_ERR_IO,"failed engine rejects new mutation");
        CHECK(!chmod(g_dir,0700),"restore owned fixture");efs_kv_lsm_close(kv);
        kv=open_store(EFS_KV_LSM_SYNC,256);CHECK(kv!=NULL,"reopen after uncertain flush");
        if(kv){CHECK(!get_is(kv,"failed0",value),"already durable WAL survives failed flush");CHECK(get_is(kv,"rejected","")==EFS_ERR_NOT_FOUND,"failed mutation remains absent");efs_kv_lsm_close(kv);}
    }else fprintf(stderr,"deferred flush real permission failure NOTRUN as root\n");
    rmtree(g_dir);
}

struct work_oracle { unsigned seen;uint8_t skip[4];uint32_t skip_len; };
static int work_oracle_cb(void *u,const uint8_t *k,uint32_t n,const uint8_t *v,uint32_t vn)
{
    struct work_oracle *o=u;
    if(n==o->skip_len && !memcmp(k,o->skip,n))return 0;
    CHECK(n==4 && vn==1 && v[0]==42,"bounded scan valid surviving row");
    o->seen++;return 0;
}
static int work_refuse_cb(void *u,const uint8_t *k,uint32_t n,const uint8_t *v,uint32_t vn)
{ (void)u;(void)k;(void)n;(void)v;(void)vn;return 2; }
static void test_tombstone_work_pages(void)
{
    rmtree(g_dir);
    struct efs_kv_lsm_cfg cfg={.sync_mode=EFS_KV_LSM_NOSYNC,.memtable_max=16*1024*1024,.l0_max=64};
    struct efs_kv *kv=efs_kv_lsm_open(g_dir,&cfg);CHECK(kv,"open work-page store");if(!kv)return;
    uint8_t key[4]={0x31,0,0,0},value=42;
    for(unsigned i=0;i<10000;i++) {key[2]=i>>8;key[3]=i;CHECK(!efs_kv_put(kv,key,4,&value,1),"seed work row");}
    CHECK(!efs_kv_lsm_flush(kv),"flush original rows");
    for(unsigned i=0;i<10000;i++)if(i%1000) {key[2]=i>>8;key[3]=i;CHECK(!efs_kv_del(kv,key,4),"seed hidden tombstone");}
    CHECK(!efs_kv_lsm_flush(kv),"flush deleted winners");
    efs_kv_lsm_close(kv);kv=efs_kv_lsm_open(g_dir,&cfg);CHECK(kv,"reopen work-page store");if(!kv)return;
    struct work_oracle o={0};uint8_t resume[EFS_KV_KEY_MAX],cursor[4];uint32_t clen=0,len;
    unsigned pages=0;int rc;
    do {
        memcpy(o.skip,cursor,clen);o.skip_len=clen;
        efs_kv_lsm_scan_stats(NULL,1);
        rc=efs_kv_lsm_scan_work(kv,key,1,clen?cursor:NULL,clen,17,work_oracle_cb,&o,resume,&len);
        struct efs_kv_scan_stats st;efs_kv_lsm_scan_stats(&st,0);
        CHECK(rc>=0 && st.keys<=17,"hidden tombstones obey key budget");
        if(rc>0) {CHECK(len==4,"tombstone-only page advances cursor");if(len!=4)break;
            CHECK(!clen || memcmp(resume,cursor,4)>0,"cursor strictly advances");memcpy(cursor,resume,4);clen=4;}
        pages++;CHECK(pages<700,"bounded scan eventually exhausts");
    } while(rc>0 && pages<700);
    CHECK(rc==0 && o.seen==10 && pages>500,"all survivors once across tombstone pages");
    rc=efs_kv_lsm_scan_work(kv,key,1,NULL,0,17,work_refuse_cb,NULL,resume,&len);
    CHECK(rc==2 && len==0,"callback refusal is not a work continuation");
    efs_kv_lsm_close(kv);rmtree(g_dir);
}

int main(void)
{
    snprintf(g_dir, sizeof(g_dir), "/tmp/efs-kvlsm-%d", (int)getpid());
    rmtree(g_dir);
    if (getenv("EFS_TEST_KV_DEFER_ONLY")) {test_deferred_held_flush();goto report;}
    test_tombstone_work_pages();
    if (getenv("EFS_TEST_KV_WORK_ONLY")) goto report;
    test_deferred_held_flush();
    if (getenv("EFS_TEST_KV_APPLY_ONLY")) {test_apply_scope();goto report;}
    if (getenv("EFS_TEST_KV_SNAPSHOT_ONLY")) {
        test_snapshot_scan_bounds();
        test_full_snapshot();
        test_snapshot_batch_cut();
        test_metadata_dirty_hints();
        test_durable_query_hints();
        goto report;
    }

    test_apply_scope();
    test_pressure_observation();
    rmtree(g_dir);
    test_semantics();
    test_reopen_wal();
    test_flush_and_levels();
    test_scan_across_levels();
    test_scan_from();
    test_torn_tail();
    test_crash_durability();
    test_bulk_auto_compact();
    test_kv_group_snap();
    test_compact_full_l0();
    test_l1_grows();
    test_flush_nowait();
    test_pinned_view();
    test_snapshot_scan_bounds();
    test_full_snapshot();
    test_snapshot_batch_cut();
    test_metadata_dirty_hints();
    test_durable_query_hints();
    test_compact_crash();
    test_sync_hold();
    test_partitioned_flush();
    test_view_export_overlap();
    test_flush_with_unapplied_wal();

report:
    rmtree(g_dir);
    if (failures) {
        fprintf(stderr, "test_kv_lsm: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_kv_lsm: OK\n");
    return 0;
}

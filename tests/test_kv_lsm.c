/* Durable ordered-KV tests. No sockets, no cluster. Uses a temp directory:
 * every case that claims durability reopens the store, and the crash case
 * really exits the process so an unfsynced page cache cannot pass for it. */
#include "efs/common.h"
#include "efs/kv.h"
#include "efs/kv_key.h"
#include "efs/kv_lsm.h"
#include "efs/kv_snap.h"
#include "efs/raft.h"

#include <dirent.h>
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

/* A torn trailing record was never ACKed, so it must be discarded while
 * everything before it survives. */
static void test_torn_tail(void)
{
    struct efs_kv *kv;
    char path[600];
    FILE *f;

    rmtree(g_dir);
    kv = open_store(EFS_KV_LSM_NOSYNC, 0);
    CHECK(kv != NULL, "open");
    if (!kv)
        return;
    CHECK(put_s(kv, "good", "yes") == EFS_OK, "put");
    efs_kv_lsm_close(kv);

    snprintf(path, sizeof(path), "%s/wal", g_dir);
    f = fopen(path, "ab");
    CHECK(f != NULL, "open wal");
    if (f) {
        static const uint8_t junk[13] = { 0xAA, 0xBB, 0xCC, 0xDD, 9, 0, 0, 0,
                                          1, 2, 3, 4, 5 };
        fwrite(junk, 1, sizeof(junk), f);
        fclose(f);
    }
    kv = open_store(EFS_KV_LSM_NOSYNC, 0);
    CHECK(kv != NULL, "reopen with torn tail");
    if (!kv)
        return;
    CHECK(get_is(kv, "good", "yes") == EFS_OK, "prefix survived");
    efs_kv_lsm_close(kv);
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
    CHECK(put_ino(src, 1, 1, "odd") == EFS_OK, "put g0");
    CHECK(put_ino(src, 2, 2, "even") == EFS_OK, "put g2");
    CHECK(efs_kv_group_export(src, EFS_RAFT_GROUP_SHARD, 4096, &blob, &blen) ==
              EFS_OK,
          "export g0");
    CHECK(blen >= 4 && blob, "blob");
    CHECK(efs_kv_group_import(dst, EFS_RAFT_GROUP_SHARD, blob, blen) == EFS_OK,
          "import g0");
    CHECK(get_ino(dst, 1, 1, "odd") == EFS_OK, "g0 landed");
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
    int i;

    for (i = 0; i < 20; i++) {
        char k[32], v[32];

        snprintf(k, sizeof(k), "extra%02d", i);
        snprintf(v, sizeof(v), "x%02d", i);
        if (put_s(kv, k, v) != EFS_OK)
            return (void *)1;
        if (efs_kv_lsm_flush(kv) != EFS_OK)
            return (void *)1;
        if (efs_kv_lsm_compact(kv) != EFS_OK)
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
            snprintf(names[n], 64, "%s", de->d_name);
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

int main(void)
{
    snprintf(g_dir, sizeof(g_dir), "/tmp/efs-kvlsm-%d", (int)getpid());
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
    test_pinned_view();
    test_compact_crash();
    test_sync_hold();
    test_partitioned_flush();

    rmtree(g_dir);
    if (failures) {
        fprintf(stderr, "test_kv_lsm: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_kv_lsm: OK\n");
    return 0;
}

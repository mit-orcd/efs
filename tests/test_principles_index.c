/* Independent W95 protocol model gates; no production engine calls. */
#include "src/bench/principles_index.h"
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#define QUOTA (4u << 20)
static void remove_owned(const char *root)
{
    DIR *d = opendir(root);
    assert(d);
    struct dirent *e;
    while ((e = readdir(d)))
        if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) {
            char p[1024];
            assert(snprintf(p, sizeof(p), "%s/%s", root, e->d_name) < (int)sizeof(p));
            assert(!unlink(p));
        }
    closedir(d);
    assert(!rmdir(root));
}
static struct principles_index *fresh(char root[64])
{
    strcpy(root, "/tmp/efs-principles-index-XXXXXX");
    assert(mkdtemp(root));
    struct principles_index *m = principles_index_open(root, 16, QUOTA, 1, 0);
    assert(m);
    return m;
}
static void expect(struct principles_index *m, unsigned id, const void *data, unsigned size,
                   uint64_t gen)
{
    unsigned n = 1048576;
    uint8_t *buf = malloc(n);
    assert(buf);
    uint64_t got = 0;
    int rc = principles_index_get(m, id, buf, &n, &got);
    if (data)
        assert(!rc && n == size && got == gen && !memcmp(buf, data, size));
    else
        assert(rc == -1 && errno == ENOENT && got == gen);
    free(buf);
}
static void crash(void *arg, enum principles_index_stage phase)
{
    if ((unsigned)phase == *(unsigned *)arg)
        _exit(88);
}
static void crash_phases(void)
{
    for (unsigned deletion = 0; deletion < 2; deletion++)
        for (unsigned phase = 1; phase <= 9; phase++) {
            char root[64];
            struct principles_index *m = fresh(root);
            struct principles_change c = {.object = 1, .data = "old", .len = 3};
            assert(!principles_index_publish(m, &c, 1));
            principles_index_close(m);
            pid_t pid = fork();
            assert(pid >= 0);
            if (!pid) {
                m = principles_index_open(root, 16, QUOTA, 0, 0);
                assert(m);
                principles_index_set_hook(m, crash, &phase);
                c.expected_generation = 1;
                c.data = deletion ? NULL : "new";
                c.len = deletion ? 0 : 3;
                assert(!principles_index_publish(m, &c, 1));
                if (phase == 9)
                    _exit(88); /* actual acknowledged return, separate from publication hook */
                uint64_t bytes;
                assert(!principles_index_collect(m, &bytes));
                _exit(2);
            }
            int status;
            assert(waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
                   WEXITSTATUS(status) == 88);
            m = principles_index_open(root, 16, QUOTA, 0, 1);
            assert(m);
            if (phase < PI_RENAME)
                expect(m, 1, "old", 3, 1);
            else
                expect(m, 1, deletion ? NULL : "new", deletion ? 0 : 3, 2);
            principles_index_close(m);
            m = principles_index_open(root, 16, QUOTA, 0, 0);
            assert(m);
            struct principles_index_stats stable;
            assert(!principles_index_stats(m, &stable) && stable.syncs >= 2);
            uint64_t bytes;
            assert(!principles_index_collect(m, &bytes));
            c.expected_generation = phase < PI_RENAME ? 1 : 2;
            c.data = "next";
            c.len = 4;
            assert(!principles_index_publish(m, &c, 1));
            expect(m, 1, "next", 4, c.expected_generation + 1);
            assert(!principles_index_collect(m, &bytes));
            principles_index_close(m);
            remove_owned(root);
        }
}
static void churn(void)
{
    char root[64];
    struct principles_index *m = fresh(root);
    uint64_t gen[16] = {0};
    int alive[16] = {0};
    uint8_t data[16][512];
    unsigned sizes[16];
    for (unsigned round = 0; round < 300; round++) {
        struct principles_change changes[16];
        unsigned n = 0;
        for (unsigned id = 0; id < 16; id++) {
            int del = alive[id] && (round + id) % 4 == 0;
            unsigned lens[] = {1, 17, 128, 512};
            sizes[id] = lens[(round + id) % 4];
            memset(data[id], (int)(round + id), sizes[id]);
            changes[n++] =
                (struct principles_change){id, del ? 0 : sizes[id], gen[id], del ? NULL : data[id]};
            alive[id] = !del;
            gen[id]++;
        }
        assert(!principles_index_publish(m, changes, n));
        for (unsigned id = 0; id < 16; id++)
            expect(m, id, alive[id] ? data[id] : NULL, alive[id] ? sizes[id] : 0, gen[id]);
        uint64_t removed;
        assert(!principles_index_collect(m, &removed));
        struct principles_index_stats s;
        assert(!principles_index_stats(m, &s));
        assert(s.allocated_bytes <= QUOTA && s.segment_bytes < 16384 && s.garbage_bytes < 16384);
        if (round % 10 == 0) {
            principles_index_close(m);
            m = principles_index_open(root, 16, QUOTA, 0, 0);
            assert(m);
        }
    }
    struct principles_change del[16];
    unsigned n = 0;
    for (unsigned id = 0; id < 16; id++)
        if (alive[id])
            del[n++] = (struct principles_change){id, 0, gen[id], NULL};
    assert(n && !principles_index_publish(m, del, n));
    uint64_t removed;
    assert(!principles_index_collect(m, &removed));
    struct principles_index_stats s;
    assert(!principles_index_stats(m, &s) && !s.live_bytes && !s.segment_bytes && !s.garbage_bytes);
    principles_index_close(m);
    m = principles_index_open(root, 16, QUOTA, 0, 1);
    assert(m);
    principles_index_close(m);
    remove_owned(root);
}
static void shared_segment_gc(void)
{
    char root[64];
    struct principles_index *m = fresh(root);
    uint8_t body[512];
    memset(body, 0x3a, sizeof(body));
    struct principles_change cohort[2] = {{10, 512, 0, body}, {11, 512, 0, body}};
    assert(!principles_index_publish(m, cohort, 2));
    struct principles_change c = {10, 0, 1, NULL};
    assert(!principles_index_publish(m, &c, 1));
    uint64_t removed;
    assert(!principles_index_collect(m, &removed) && !removed);
    struct principles_index_stats st;
    assert(!principles_index_stats(m, &st));
    assert(st.live_bytes == 512 && st.garbage_bytes == 564 && st.segment_bytes == 1152);
    expect(m, 11, body, 512, 1);
    expect(m, 10, NULL, 0, 2);
    assert(principles_index_publish(m, &c, 1) == -1 && errno == ESTALE);
    c = (struct principles_change){10, 3, 2, "new"};
    assert(!principles_index_publish(m, &c, 1));
    assert(!principles_index_collect(m, &removed) && !removed);
    expect(m, 11, body, 512, 1);
    c = (struct principles_change){11, 0, 1, NULL};
    assert(!principles_index_publish(m, &c, 1));
    assert(!principles_index_collect(m, &removed) && removed);
    expect(m, 10, "new", 3, 3);
    principles_index_close(m);
    m = principles_index_open(root, 16, QUOTA, 0, 1);
    assert(m);
    expect(m, 10, "new", 3, 3);
    expect(m, 11, NULL, 0, 2);
    principles_index_close(m);
    remove_owned(root);
}
static void refusals_and_corruption(void)
{
    char root[64];
    struct principles_index *m = fresh(root);
    struct principles_change c = {0, 3, 0, "abc"};
    assert(!principles_index_publish(m, &c, 1));
    c.expected_generation = 0;
    c.data = NULL;
    c.len = 0;
    assert(principles_index_publish(m, &c, 1) == -1 && errno == ESTALE);
    expect(m, 0, "abc", 3, 1);
    assert(!principles_index_open(root, 16, QUOTA, 0, 0)); /* exclusive writer */
    c = (struct principles_change){0, 3, 1, "xyz"};
    assert(!principles_index_publish(m, &c, 1));
    expect(m, 0, "xyz", 3, 2);
    c = (struct principles_change){0, 0, 1, NULL};
    assert(principles_index_publish(m, &c, 1) == -1 && errno == ESTALE);
    expect(m, 0, "xyz", 3, 2);
    uint64_t removed;
    assert(!principles_index_collect(m, &removed) && removed > 0);
    principles_index_close(m);
    m = principles_index_open(root, 16, 4096, 0, 0);
    assert(m);
    c = (struct principles_change){0, 3, 2, "cap"};
    assert(principles_index_publish(m, &c, 1) == -1 && errno == ENOSPC);
    expect(m, 0, "xyz", 3, 2);
    principles_index_close(m);
    /* Corrupt live body evidence: cold recovery must fail, preserve everything. */
    char path[1024];
    snprintf(path, sizeof(path), "%s/seg-%016x", root, 2);
    int fd = open(path, O_RDWR);
    assert(fd >= 0);
    uint8_t bad = 0xff;
    assert(pwrite(fd, &bad, 1, 45) == 1);
    assert(!fsync(fd));
    close(fd);
    assert(!principles_index_open(root, 16, QUOTA, 0, 1));
    struct stat st;
    assert(!stat(path, &st));
    remove_owned(root);
    m = fresh(root);
    principles_index_close(m);
    snprintf(path, sizeof(path), "%s/.index-manifest", root);
    fd = open(path, O_RDWR);
    assert(fd >= 0);
    assert(pwrite(fd, &bad, 1, 20) == 1);
    assert(!fsync(fd));
    close(fd);
    assert(!principles_index_open(root, 16, QUOTA, 0, 0));
    assert(!stat(path, &st));
    remove_owned(root);
}
int main(void)
{
    crash_phases();
    churn();
    shared_segment_gc();
    refusals_and_corruption();
    puts("indexed principles: 18 crash points, conditional identity, 4800 churn operations, cold "
         "bytes/quota/physical GC and corrupt-evidence refusal PASS");
    return 0;
}

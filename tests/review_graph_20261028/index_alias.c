/* Reproduce a current defect, not a passing product regression. */
#include "efs/raft.h"
#include "efs/raft_disk.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int alias_read(struct efs_raft_store *s, const char *label)
{
    uint8_t out[8] = {0}; uint32_t len = sizeof(out); uint64_t term = 0;
    uint64_t alias = UINT64_C(1) + (UINT64_C(1) << 32);
    int rc = s->get(s, alias, &term, out, &len);
    printf("%s get(%llu): rc=%d term=%llu len=%u byte=%c; expected NOT_FOUND=%d\n",
           label, (unsigned long long)alias, rc, (unsigned long long)term, len, out[0], EFS_ERR_NOT_FOUND);
    return rc == EFS_OK && term == 7 && len == 1 && out[0] == 'A';
}
int main(int argc, char **argv)
{
    assert(argc == 2 || argc == 3);
    int fixed = argc == 3 && !strcmp(argv[2], "fixed");
    struct efs_raft_store *m = efs_raft_mem_create(); assert(m);
    assert(m->append(m, 1, 7, (const uint8_t *)"A", 1) == EFS_OK);
    int found = alias_read(m, "memory");
    struct efs_raft_disk *d = efs_raft_disk_open(argv[1], EFS_RAFT_DISK_NOSYNC); assert(d);
    struct efs_raft_store *s = efs_raft_disk_group(d, 0); assert(s);
    assert(s->append(s, 1, 7, (const uint8_t *)"A", 1) == EFS_OK);
    found += alias_read(s, "disk-warm");
    /* Normal 64-bit append bounds already reject the same invalid index. */
    int rc = s->append(s, UINT64_C(1) + (UINT64_C(1) << 32), 8, (const uint8_t *)"B", 1);
    printf("disk append alias control: rc=%d (expected INVAL=%d)\n", rc, EFS_ERR_INVAL);
    assert(rc == EFS_ERR_INVAL);
    efs_raft_disk_close(d);
    d = efs_raft_disk_open(argv[1], EFS_RAFT_DISK_NOSYNC); assert(d);
    s = efs_raft_disk_group(d, 0); assert(s);
    found += alias_read(s, "disk-reopened");
    efs_raft_disk_close(d); efs_raft_mem_free(m);
    printf("INDEX_ALIAS_CONFIRMED=%d\n", found == 3);
    return found == (fixed ? 0 : 3) ? 0 : 1;
}

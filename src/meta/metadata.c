#include "efs/metadata.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define EFS_META_MAGIC "EFSM"
#define EFS_META_ROOT_MAGIC "EFSR"
/* EFSR version constants (EFS_META_ROOT_VERSION_V1..V7, EFS_META_ROOT_VERSION)
 * live in metadata.h (shared with the client flush). */
/* EFSM: v2: uid/gid. v3: mtime_nsec after mtime. v4: atime + dir rollups.
 * v5: fixed-size inode records + pack fields; chunk table is a separate
 * page region so creates do not shift chunk pages.
 * v6: compact inodes (no name / tree rollups) + packed dentries.
 * v7: dentry region page-aligned within the inode region (offset derived
 * from inode_count) so a create no longer shifts/re-dirties the dentry
 * tail — O(1) metadata flush per create instead of O(table).
 * EFS_META_VERSION is in metadata.h (shared with the client incremental
 * serialize). */
/* EFS_INODE_WIRE_SIZE is in metadata.h (EFSM v5). */

static size_t dentry_rec_len(const char *name);
static void dentry_bytes_add(struct efs_export *ex, const char *name);
static void dentry_bytes_sub(struct efs_export *ex, const char *name);

static void time_now(uint64_t *t)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    *t = (uint64_t)ts.tv_sec;
}

/* POSIX link/rename bump ctime. Second resolution would hide a same-second
 * link, so advance by 1 when the clock has not moved. */
static void inode_bump_ctime(struct efs_inode_mem *p)
{
    if (!p)
        return;
    uint64_t old = p->ctime;
    time_now(&p->ctime);
    if (p->ctime <= old)
        p->ctime = old + 1;
}

static struct efs_inode_mem *inode_ptr(struct efs_export *ex, efs_ino_t ino);
static struct efs_inode_mem *inode_at(struct efs_export *ex, uint64_t slot);
static int inode_slab_fault(struct efs_export *ex, uint32_t si);
static int inode_slab_ensure(struct efs_export *ex, uint32_t si);
static int inode_ensure_cap(struct efs_export *ex, uint64_t need);
static int inode_set_name(struct efs_export *ex, struct efs_inode_mem *p,
                          const char *name);
static uint32_t slab_of_row(const struct efs_export *ex,
                            const struct efs_inode_mem *p);
static void remove_inode_slot(struct efs_export *ex, uint64_t i,
                              int expect_survivor);

static int slab_names_grow(struct efs_ino_slab *sl, uint32_t need)
{
    uint32_t ncap;
    char *na;
    if (!sl)
        return -1;
    if (sl->names_used + need <= sl->names_cap)
        return 0;
    ncap = sl->names_cap ? sl->names_cap : 1024;
    while (ncap < sl->names_used + need) {
        if (ncap > UINT32_MAX / 2)
            return -1;
        ncap *= 2;
    }
    na = realloc(sl->names, ncap);
    if (!na)
        return -1;
    sl->names = na;
    sl->names_cap = ncap;
    return 0;
}

/* Rewrite the slab's arena keeping only live names. inode_set_name appends
 * whenever a new name is longer than the old one, so a rename-heavy slab
 * would otherwise grow without bound. A slab is EFS_INO_SLAB_ROWS rows, so
 * this is cheap and strictly local. */
static void slab_names_compact(struct efs_export *ex, uint32_t si)
{
    struct efs_ino_slab *sl;
    char *fresh;
    uint32_t used = 0, k;
    if (!ex || !ex->ino_slabs || si >= ex->ino_slab_n)
        return;
    sl = &ex->ino_slabs[si];
    if (!sl->rows || !sl->names || !sl->names_cap)
        return;
    fresh = malloc(sl->names_cap);
    if (!fresh)
        return;
    for (k = 0; k < EFS_INO_SLAB_ROWS; k++) {
        struct efs_inode_mem *row = &sl->rows[k];
        uint32_t ln = row->name_len;
        if (!ln)
            continue;
        if ((uint64_t)row->name_off + ln > sl->names_used ||
            (uint64_t)used + ln + 1 > sl->names_cap) {
            row->name_len = 0;
            row->name_off = 0;
            continue;
        }
        memcpy(fresh + used, sl->names + row->name_off, ln);
        fresh[used + ln] = '\0';
        row->name_off = used;
        used += ln + 1;
    }
    free(sl->names);
    sl->names = fresh;
    sl->names_used = used;
}

static int inode_ensure_cap(struct efs_export *ex, uint64_t need)
{
    uint32_t want;
    if (!ex)
        return EFS_ERR_INVAL;
    if (need < 1)
        need = 1;
    want = (uint32_t)((need + EFS_INO_SLAB_ROWS - 1) / EFS_INO_SLAB_ROWS);
    if (want < 1)
        want = 1;
    if (want > ex->ino_slab_n) {
        struct efs_ino_slab *n = realloc(ex->ino_slabs, want * sizeof(*n));
        if (!n)
            return EFS_ERR_NOMEM;
        memset(n + ex->ino_slab_n, 0,
               (want - ex->ino_slab_n) * sizeof(*n));
        ex->ino_slabs = n;
        ex->ino_slab_n = want;
    }
    /* Grow the slab directory only. Rows are faulted or calloc'd on access so
     * a RAM cap can drop clean slabs; realloc must not re-materialize them. */
    ex->inode_capacity = (uint64_t)ex->ino_slab_n * EFS_INO_SLAB_ROWS;
    if (ex->inodes) {
        struct efs_inode_mem *old = ex->inodes;
        uint64_t n = ex->inode_count;
        uint64_t i;
        ex->inodes = NULL;
        for (i = 0; i < n; i++) {
            struct efs_inode_mem *d = inode_at(ex, i);
            if (!d) {
                free(old);
                return EFS_ERR_NOMEM;
            }
            *d = old[i];
        }
        free(old);
    }
    return EFS_OK;
}

/* Decode one EFSM v8 row (124 B payload, u16 name_len, inline name). */
void efs_export_unpack_row(struct efs_export *ex, struct efs_inode_mem *row,
                           const uint8_t *p)
{
    uint16_t ln = 0;
    char nbuf[EFS_MAX_NAME];
    uint16_t slab = row->slab_idx;
    memset(row, 0, sizeof(*row));
    row->slab_idx = slab;
    memcpy(&row->ino, p, 8); p += 8;
    memcpy(&row->parent, p, 8); p += 8;
    memcpy(&row->mode, p, 4); p += 4;
    {
        uint32_t uid, gid;
        memcpy(&uid, p, 4); p += 4;
        memcpy(&gid, p, 4); p += 4;
        row->uid = (uid_t)uid;
        row->gid = (gid_t)gid;
    }
    memcpy(&row->size, p, 8); p += 8;
    memcpy(&row->mtime, p, 8); p += 8;
    memcpy(&row->mtime_nsec, p, 4); p += 4;
    memcpy(&row->ctime, p, 8); p += 8;
    memcpy(&row->atime, p, 8); p += 8;
    memcpy(&row->nlink, p, 4); p += 4;
    memcpy(&row->imm_files, p, 8); p += 8;
    memcpy(&row->imm_dirs, p, 8); p += 8;
    memcpy(&row->imm_bytes, p, 8); p += 8;
    memcpy(&row->imm_tmin, p, 8); p += 8;
    memcpy(&row->imm_tmax, p, 8); p += 8;
    memcpy(&row->pack_ino, p, 8); p += 8;
    memcpy(&row->pack_off, p, 4); p += 4;
    memcpy(&row->pack_len, p, 4); p += 4;
    memcpy(&ln, p, 2); p += 2;
    if (ln >= EFS_MAX_NAME)
        ln = EFS_MAX_NAME - 1;
    if (!ln)
        return;
    memset(nbuf, 0, sizeof(nbuf));
    memcpy(nbuf, p, ln);
    inode_set_name(ex, row, nbuf);
}

/* Restore slab si from a serialized image of its page. v8 pins slab si to
 * page 1+si and carries each row's name inline, so this reads one page and
 * needs nothing else -- that is what lets a fault come from the CoW
 * fragments instead of a full in-RAM copy of the table. */
static int inode_slab_fault_from_page(struct efs_export *ex, uint32_t si,
                                      const uint8_t *page)
{
    uint64_t s0, s1, slot;
    if (!ex || si >= ex->ino_slab_n || !page)
        return -1;
    if (!ex->ino_slabs[si].rows) {
        ex->ino_slabs[si].rows = calloc(EFS_INO_SLAB_ROWS,
                                        sizeof(struct efs_inode_mem));
        if (!ex->ino_slabs[si].rows)
            return -1;
        ex->ino_slabs_resident++;
    }
    s0 = (uint64_t)si * EFS_INO_SLAB_ROWS;
    s1 = s0 + EFS_INO_SLAB_ROWS;
    if (s1 > ex->inode_count)
        s1 = ex->inode_count;
    for (slot = s0; slot < s1; slot++) {
        struct efs_inode_mem *row = &ex->ino_slabs[si].rows[slot - s0];
        row->slab_idx = (uint16_t)si;
        efs_export_unpack_row(ex, row,
                              page + (slot - s0) * EFS_INODE_ROW_SIZE);
    }
    ex->ino_slabs[si].tick = ++ex->ino_slab_tick;
    return 0;
}

static int inode_slab_fault(struct efs_export *ex, uint32_t si)
{
    uint32_t pi;
    size_t off;
    if (!ex || si >= ex->ino_slab_n)
        return -1;
    if (ex->ino_slabs[si].rows)
        return 0;
    pi = si + 1; /* page 0 is the header */
    off = (size_t)pi * EFS_META_PAGE_SIZE;
    if (ex->flush_blob &&
        off + EFS_META_PAGE_SIZE <= ex->flush_blob_ino_len)
        return inode_slab_fault_from_page(
            ex, si, (const uint8_t *)ex->flush_blob + off);
    if (ex->page_src) {
        uint8_t *page = malloc(EFS_META_PAGE_SIZE);
        int rc;
        if (!page)
            return -1;
        rc = ex->page_src(ex->page_src_ctx, ex, pi, page);
        if (rc == EFS_OK)
            rc = inode_slab_fault_from_page(ex, si, page);
        else
            rc = -1;
        free(page);
        return rc;
    }
    return -1;
}

static void slab_rows_tag(struct efs_export *ex, uint32_t si)
{
    uint32_t k;
    for (k = 0; k < EFS_INO_SLAB_ROWS; k++)
        ex->ino_slabs[si].rows[k].slab_idx = (uint16_t)si;
}

/* Does the persisted image actually carry this slab's page? A slab past the
 * end of it was never written, so a failed fault there means "new growth",
 * not "the rows were evicted and are gone". */
static int slab_page_persisted(const struct efs_export *ex, uint32_t si)
{
    size_t off = (size_t)(si + 1) * EFS_META_PAGE_SIZE;
    uint32_t ino_pc;
    if (ex->flush_blob)
        return off + EFS_META_PAGE_SIZE <= ex->flush_blob_ino_len;
    /* No blob: the committed root is the authority on which pages the image
     * covers, and slab si is exactly page 1+si. This must NOT be answered
     * with "a page source exists" — that is true for every si the moment one
     * is installed, so the first row of each new slab (callers do
     * pos = inode_count++ before filling it, which already makes it look like
     * a fault) would become a hard failure instead of fresh growth. That is
     * the "table jammed at exactly 256 rows" bug. */
    ino_pc = ex->root.ino_page_count ? ex->root.ino_page_count
                                     : ex->root.page_count;
    return ino_pc && (uint64_t)si + 1 < (uint64_t)ino_pc;
}

static int inode_slab_ensure(struct efs_export *ex, uint32_t si)
{
    uint64_t s0;
    if (!ex || !ex->ino_slabs || si >= ex->ino_slab_n)
        return -1;
    if (ex->ino_slabs[si].rows)
        return 0;
    s0 = (uint64_t)si * EFS_INO_SLAB_ROWS;
    /* Only treat this as a fault when something can actually restore the
     * rows. A snapshot target sets inode_count up front and then fills the
     * rows itself, so it must get a fresh zeroed slab, not a failure. */
    if (s0 < ex->inode_count && (ex->flush_blob || ex->page_src)) {
        if (inode_slab_fault(ex, si) == 0)
            return 0;
        /* Live rows were evicted and no page source can restore them. A
         * slab the image never covered is instead the first row of fresh
         * growth: callers do pos = inode_count++ before filling the row, so
         * s0 < inode_count is already true for it. Fall through to calloc. */
        if (slab_page_persisted(ex, si))
            return -1;
    }
    ex->ino_slabs[si].rows = calloc(EFS_INO_SLAB_ROWS,
                                    sizeof(struct efs_inode_mem));
    if (!ex->ino_slabs[si].rows)
        return -1;
    ex->ino_slabs_resident++;
    slab_rows_tag(ex, si);
    ex->ino_slabs[si].tick = ++ex->ino_slab_tick;
    return 0;
}

static struct efs_inode_mem *inode_at(struct efs_export *ex, uint64_t slot)
{
    uint32_t si, off;
    if (!ex)
        return NULL;
    if (ex->inodes) {
        if (slot >= ex->inode_capacity)
            return NULL;
        return &ex->inodes[slot];
    }
    if (!ex->ino_slabs || slot >= ex->inode_capacity)
        return NULL;
    si = (uint32_t)(slot / EFS_INO_SLAB_ROWS);
    off = (uint32_t)(slot % EFS_INO_SLAB_ROWS);
    if (si >= ex->ino_slab_n)
        return NULL;
    if (inode_slab_ensure(ex, si) != 0)
        return NULL;
    if (!ex->ino_slabs[si].rows)
        return NULL;
    ex->ino_slabs[si].tick = ++ex->ino_slab_tick;
    /* Stamp on every access: callers memset rows before filling them, and a
     * row whose slab_idx is stale would resolve its name against the wrong
     * arena. One store is cheaper than auditing every memset. */
    ex->ino_slabs[si].rows[off].slab_idx = (uint16_t)si;
    return &ex->ino_slabs[si].rows[off];
}

static uint64_t inode_slot_of(const struct efs_export *ex,
                              const struct efs_inode_mem *p)
{
    uint32_t si;
    if (!ex || !p)
        return (uint64_t)-1;
    if (ex->inodes) {
        if (p < ex->inodes)
            return (uint64_t)-1;
        return (uint64_t)(p - ex->inodes);
    }
    si = slab_of_row(ex, p);
    if (si != (uint32_t)-1) {
        struct efs_inode_mem *r = ex->ino_slabs[si].rows;
        return (uint64_t)si * EFS_INO_SLAB_ROWS + (uint64_t)(p - r);
    }
    return (uint64_t)-1;
}

/* Resolve a row to its slab. slab_idx is the O(1) answer, but callers zero
 * rows before filling them (memset clears the stamp), and a wrong slab would
 * silently read another slab's arena. Verify the pointer really lies in that
 * slab and fall back to a scan that repairs the stamp. */
static uint32_t slab_of_row(const struct efs_export *ex,
                            const struct efs_inode_mem *p)
{
    uint32_t si;
    if (!ex || !p || !ex->ino_slabs)
        return (uint32_t)-1;
    si = p->slab_idx;
    if (si < ex->ino_slab_n) {
        struct efs_inode_mem *r = ex->ino_slabs[si].rows;
        if (r && p >= r && p < r + EFS_INO_SLAB_ROWS)
            return si;
    }
    for (si = 0; si < ex->ino_slab_n; si++) {
        struct efs_inode_mem *r = ex->ino_slabs[si].rows;
        if (r && p >= r && p < r + EFS_INO_SLAB_ROWS) {
            ((struct efs_inode_mem *)p)->slab_idx = (uint16_t)si;
            return si;
        }
    }
    return (uint32_t)-1;
}

static const char *inamep(const struct efs_export *ex, const struct efs_inode_mem *p)
{
    const struct efs_ino_slab *sl;
    uint32_t si;
    if (!ex || !p || !p->name_len)
        return "";
    si = slab_of_row(ex, p);
    if (si == (uint32_t)-1)
        return "";
    sl = &ex->ino_slabs[si];
    if (!sl->names || (uint64_t)p->name_off + p->name_len > sl->names_used)
        return "";
    return sl->names + p->name_off;
}

const char *efs_export_inode_name(const struct efs_export *ex, uint64_t slot)
{
    struct efs_inode_mem *p;
    if (!ex || slot >= ex->inode_count)
        return "";
    p = inode_at((struct efs_export *)ex, slot);
    if (!p)
        return "";
    return inamep(ex, p);
}

static void inode_copy_attr(struct efs_inode_mem *d, const struct efs_inode *s)
{
    d->ino = s->ino;
    d->parent = s->parent;
    d->mode = s->mode;
    d->uid = s->uid;
    d->gid = s->gid;
    d->size = s->size;
    d->mtime = s->mtime;
    d->mtime_nsec = s->mtime_nsec;
    d->ctime = s->ctime;
    d->atime = s->atime;
    d->nlink = s->nlink;
    d->imm_files = s->imm_files;
    d->imm_dirs = s->imm_dirs;
    d->tree_files = s->tree_files;
    d->tree_dirs = s->tree_dirs;
    d->imm_bytes = s->imm_bytes;
    d->tree_bytes = s->tree_bytes;
    d->imm_tmin = s->imm_tmin;
    d->imm_tmax = s->imm_tmax;
    d->tree_tmin = s->tree_tmin;
    d->tree_tmax = s->tree_tmax;
    d->pack_ino = s->pack_ino;
    d->pack_off = s->pack_off;
    d->pack_len = s->pack_len;
}

static void inode_to_rpc_p(const struct efs_export *ex,
                           const struct efs_inode_mem *p, struct efs_inode *out)
{
    memset(out, 0, sizeof(*out));
    if (!p)
        return;
    out->ino = p->ino;
    out->parent = p->parent;
    out->mode = p->mode;
    out->uid = p->uid;
    out->gid = p->gid;
    out->size = p->size;
    out->mtime = p->mtime;
    out->mtime_nsec = p->mtime_nsec;
    out->ctime = p->ctime;
    out->atime = p->atime;
    out->nlink = p->nlink;
    out->imm_files = p->imm_files;
    out->imm_dirs = p->imm_dirs;
    out->tree_files = p->tree_files;
    out->tree_dirs = p->tree_dirs;
    out->imm_bytes = p->imm_bytes;
    out->tree_bytes = p->tree_bytes;
    out->imm_tmin = p->imm_tmin;
    out->imm_tmax = p->imm_tmax;
    out->tree_tmin = p->tree_tmin;
    out->tree_tmax = p->tree_tmax;
    out->pack_ino = p->pack_ino;
    out->pack_off = p->pack_off;
    out->pack_len = p->pack_len;
    if (p->name_len) {
        size_t n = p->name_len < EFS_MAX_NAME - 1 ? p->name_len : EFS_MAX_NAME - 1;
        memcpy(out->name, inamep(ex, p), n);
        out->name[n] = '\0';
    }
}

void efs_export_inode_to_rpc(const struct efs_export *ex, uint64_t slot,
                             struct efs_inode *out)
{
    if (!out)
        return;
    if (!ex || slot >= ex->inode_count) {
        memset(out, 0, sizeof(*out));
        return;
    }
    {
        struct efs_inode_mem *p = inode_at((struct efs_export *)ex, slot);
        if (!p) {
            memset(out, 0, sizeof(*out));
            return;
        }
        inode_to_rpc_p(ex, p, out);
    }
}

static int inode_set_name(struct efs_export *ex, struct efs_inode_mem *p,
                          const char *name)
{
    size_t ln = name ? strnlen(name, EFS_MAX_NAME - 1) : 0;
    struct efs_ino_slab *sl;
    uint32_t si;
    if (!ex || !p)
        return -1;
    si = slab_of_row(ex, p);
    if (si == (uint32_t)-1)
        return -1;
    sl = &ex->ino_slabs[si];
    /* name_off only means something once the row owns arena bytes. A row with
     * name_len 0 (fresh, or just moved between slots) owns none, so reusing
     * its offset would truncate whatever name lives there. */
    if (!ln && !p->name_len) {
        p->name_off = 0;
        return 0;
    }
    if (p->name_len && p->name_len >= ln && sl->names &&
        (uint64_t)p->name_off + p->name_len <= sl->names_used) {
        if (ln)
            memcpy(sl->names + p->name_off, name, ln);
        sl->names[p->name_off + ln] = '\0';
        p->name_len = (uint16_t)ln;
        return 0;
    }
    /* Appending past a few times the slab's live name bytes means renames
     * have orphaned most of the arena; reclaim before growing again. */
    if (sl->names_used > 64 * 1024 &&
        sl->names_used > 4 * EFS_INO_SLAB_ROWS * 32)
        slab_names_compact(ex, si);
    if (slab_names_grow(sl, (uint32_t)ln + 1) != 0)
        return -1;
    p->name_off = sl->names_used;
    p->name_len = (uint16_t)ln;
    if (ln)
        memcpy(sl->names + sl->names_used, name, ln);
    sl->names[sl->names_used + ln] = '\0';
    sl->names_used += (uint32_t)ln + 1;
    return 0;
}

/* Move a row to another slot. name_off is an offset into the OWNING slab's
 * name arena and slots 256 apart live in different slabs, so a raw struct
 * copy leaves the destination reading — and, on the next rename, overwriting
 * — an unrelated slab's bytes. Re-intern the name into the destination's own
 * arena. Callers must refresh the indexes; this only moves the row. */
static int inode_row_move(struct efs_export *ex, uint64_t dst_slot,
                          uint64_t src_slot)
{
    struct efs_inode_mem tmp;
    struct efs_inode_mem *p;
    char nm[EFS_MAX_NAME];

    p = inode_at(ex, src_slot);
    if (!p)
        return -1;
    tmp = *p;
    strncpy(nm, inamep(ex, p), sizeof(nm) - 1);
    nm[sizeof(nm) - 1] = '\0';

    /* Fetch dst after reading src: inode_at can fault a slab in. */
    p = inode_at(ex, dst_slot);
    if (!p)
        return -1;
    *p = tmp;
    p->slab_idx = (uint16_t)(dst_slot / EFS_INO_SLAB_ROWS);
    p->name_off = 0;
    p->name_len = 0;
    return inode_set_name(ex, p, nm);
}

static int inode_from_rpc(struct efs_export *ex, struct efs_inode_mem *d,
                          const struct efs_inode *s)
{
    inode_copy_attr(d, s);
    /* SETATTR/REPORT replies often omit the name. Do not intern "" — that
     * zeros name_len and makes LOOKUP miss the live dentry. */
    if (!s->name[0] && d->name_len)
        return 0;
    return inode_set_name(ex, d, s->name);
}

static void stamp_ctime_loaded(struct efs_export *ex, efs_ino_t ino, uint64_t ct)
{
    if (!ex || !ino)
        return;
    /* Common case (rsync temp→final, nlink=1): the row we already hold.
     * The old full-table scan was O(inodes) per rename and made a grown
     * export's rsync O(n²). */
    struct efs_inode_mem *p = inode_ptr(ex, ino);
    if (p) {
        p->ctime = ct;
        if (p->nlink <= 1)
            return;
    }
    for (uint64_t i = 0; i < ex->inode_count; i++) {
        if (inode_at(ex, i)->ino == ino) {
            inode_at(ex, i)->ctime = ct;
        }
    }
    if (!ex->shard_tabs)
        return;
    for (uint32_t s = 1; s < ex->shard_tab_cap; s++) {
        struct efs_export *t = ex->shard_tabs[s];
        if (!t)
            continue;
        for (uint64_t i = 0; i < t->inode_count; i++) {
            if (inode_at(t, i)->ino == ino) {
                inode_at(t, i)->ctime = ct;
            }
        }
    }
}

static void parent_touch(struct efs_export *ex, efs_ino_t parent)
{
    struct efs_inode_mem *p = inode_ptr(ex, parent);
    if (!p)
        return;
    time_now(&p->mtime);
    p->ctime = p->mtime;
    p->mtime_nsec = 0;
}

static uint64_t hash_mix(uint64_t x)
{
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x ? x : 1;
}

static int export_is_sharded_root(struct efs_export *ex);

static uint64_t hash_name_key(efs_ino_t parent, const char *name)
{
    uint64_t h = hash_mix(parent);
    for (const unsigned char *p = (const unsigned char *)name; *p; p++)
        h = h * 131u + *p;
    return h ? h : 1;
}

static uint64_t hash_chunk_key(efs_ino_t ino, uint32_t chunk_index)
{
    return hash_mix(ino ^ ((uint64_t)chunk_index * 0x9E3779B97F4A7C15ULL));
}

static int idx_init(uint64_t **keys, uint64_t **vals, uint64_t *mask, uint64_t n_hint)
{
    uint64_t cap = 16;
    while (cap < n_hint * 2)
        cap *= 2;
    free(*keys);
    free(*vals);
    *keys = calloc(cap, sizeof(uint64_t));
    *vals = calloc(cap, sizeof(uint64_t));
    if (!*keys || !*vals) {
        free(*keys);
        free(*vals);
        *keys = *vals = NULL;
        *mask = 0;
        return -1;
    }
    *mask = cap - 1;
    return 0;
}

static void idx_free(uint64_t **keys, uint64_t **vals, uint64_t *mask)
{
    free(*keys);
    free(*vals);
    *keys = *vals = NULL;
    *mask = 0;
}

/* Mix before taking the slot. Client inos are (tag<<40)|counter; without a
 * mix, tag sits above a 22-bit mask so every namespace lands on `counter` and
 * a second-tree lookup walks the whole occupied run (1.3M probes, ~60% CPU). */
static uint64_t idx_slot(uint64_t key, uint64_t mask)
{
    return hash_mix(key) & mask;
}

static int idx_put(uint64_t *keys, uint64_t *vals, uint64_t mask, uint64_t key, uint64_t val)
{
    if (!keys || mask == 0 || key == 0)
        return -1;
    uint64_t i = idx_slot(key, mask);
    for (uint64_t n = 0; n <= mask; n++) {
        if (keys[i] == 0 || keys[i] == key) {
            keys[i] = key;
            vals[i] = val;
            return 0;
        }
        i = (i + 1) & mask;
    }
    return -1;
}

static int idx_get(const uint64_t *keys, const uint64_t *vals, uint64_t mask,
                   uint64_t key, uint64_t *val)
{
    if (!keys || mask == 0 || key == 0)
        return -1;
    uint64_t i = idx_slot(key, mask);
    for (uint64_t n = 0; n <= mask; n++) {
        if (keys[i] == 0)
            return -1;
        if (keys[i] == key) {
            *val = vals[i];
            return 0;
        }
        i = (i + 1) & mask;
    }
    return -1;
}

/* Tombstone-free delete: remove key and re-insert the probe chain after it. */
static void idx_del(uint64_t *keys, uint64_t *vals, uint64_t mask, uint64_t key)
{
    if (!keys || mask == 0 || key == 0)
        return;
    uint64_t i = idx_slot(key, mask);
    for (uint64_t n = 0; n <= mask; n++) {
        if (keys[i] == 0)
            return;
        if (keys[i] == key)
            break;
        i = (i + 1) & mask;
    }
    if (keys[i] != key)
        return;
    keys[i] = 0;
    vals[i] = 0;
    uint64_t j = (i + 1) & mask;
    while (keys[j] != 0) {
        uint64_t k = keys[j];
        uint64_t v = vals[j];
        keys[j] = 0;
        vals[j] = 0;
        idx_put(keys, vals, mask, k, v);
        j = (j + 1) & mask;
    }
}

/* Name/chunk indexes may share a hash across distinct keys; probe past
 * hash matches that fail (parent,name) / (ino,chunk_index) verification. */
static int name_idx_put(struct efs_export *ex, efs_ino_t parent, const char *name,
                        uint64_t pos)
{
    /* nlink=0 open-fd ghosts keep the inode row but have no directory name. */
    if (!name || !name[0] || parent == 0)
        return -1;
    if (!ex->name_keys || ex->name_mask == 0)
        return -1;
    uint64_t key = hash_name_key(parent, name);
    uint64_t i = key & ex->name_mask;
    for (uint64_t n = 0; n <= ex->name_mask; n++) {
        if (ex->name_keys[i] == 0) {
            ex->name_keys[i] = key;
            ex->name_vals[i] = pos;
            return 0;
        }
        if (ex->name_keys[i] == key) {
            uint64_t p = ex->name_vals[i];
            if (p < ex->inode_count &&
                inode_at(ex, p)->parent == parent &&
                strcmp(efs_export_inode_name(ex, p), name) == 0) {
                ex->name_vals[i] = pos;
                return 0;
            }
        }
        i = (i + 1) & ex->name_mask;
    }
    return -1;
}

static int name_idx_get(struct efs_export *ex, efs_ino_t parent, const char *name,
                        uint64_t *pos)
{
    if (!ex->name_keys || ex->name_mask == 0)
        return -1;
    uint64_t key = hash_name_key(parent, name);
    uint64_t i = key & ex->name_mask;
    for (uint64_t n = 0; n <= ex->name_mask; n++) {
        if (ex->name_keys[i] == 0)
            return -1;
        if (ex->name_keys[i] == key) {
            uint64_t p = ex->name_vals[i];
            if (p < ex->inode_count &&
                inode_at(ex, p)->parent == parent &&
                strcmp(efs_export_inode_name(ex, p), name) == 0) {
                *pos = p;
                return 0;
            }
        }
        i = (i + 1) & ex->name_mask;
    }
    return -1;
}

static void name_idx_del(struct efs_export *ex, efs_ino_t parent, const char *name)
{
    if (!ex->name_keys || ex->name_mask == 0)
        return;
    uint64_t key = hash_name_key(parent, name);
    uint64_t i = key & ex->name_mask;
    for (uint64_t n = 0; n <= ex->name_mask; n++) {
        if (ex->name_keys[i] == 0)
            return;
        if (ex->name_keys[i] == key) {
            uint64_t p = ex->name_vals[i];
            if (p < ex->inode_count &&
                inode_at(ex, p)->parent == parent &&
                strcmp(efs_export_inode_name(ex, p), name) == 0)
                break;
        }
        i = (i + 1) & ex->name_mask;
    }
    if (ex->name_keys[i] != key)
        return;
    /* Confirm identity again in case the loop exited on capacity. */
    {
        uint64_t p = ex->name_vals[i];
        if (!(p < ex->inode_count &&
              inode_at(ex, p)->parent == parent &&
              strcmp(efs_export_inode_name(ex, p), name) == 0))
            return;
    }
    ex->name_keys[i] = 0;
    ex->name_vals[i] = 0;
    uint64_t j = (i + 1) & ex->name_mask;
    while (ex->name_keys[j] != 0) {
        uint64_t k = ex->name_keys[j];
        uint64_t v = ex->name_vals[j];
        ex->name_keys[j] = 0;
        ex->name_vals[j] = 0;
        if (v < ex->inode_count)
            name_idx_put(ex, inode_at(ex, v)->parent, efs_export_inode_name(ex, v), v);
        else
            idx_put(ex->name_keys, ex->name_vals, ex->name_mask, k, v);
        j = (j + 1) & ex->name_mask;
    }
}

static int chunk_idx_put(struct efs_export *ex, efs_ino_t ino, uint32_t chunk_index,
                         uint64_t pos)
{
    if (!ex->chunk_keys || ex->chunk_mask == 0)
        return -1;
    uint64_t key = hash_chunk_key(ino, chunk_index);
    uint64_t i = key & ex->chunk_mask;
    for (uint64_t n = 0; n <= ex->chunk_mask; n++) {
        if (ex->chunk_keys[i] == 0) {
            ex->chunk_keys[i] = key;
            ex->chunk_vals[i] = pos;
            return 0;
        }
        if (ex->chunk_keys[i] == key) {
            uint64_t p = ex->chunk_vals[i];
            if (p < ex->chunk_count &&
                ex->chunks[p].ino == ino &&
                ex->chunks[p].chunk_index == chunk_index) {
                ex->chunk_vals[i] = pos;
                return 0;
            }
        }
        i = (i + 1) & ex->chunk_mask;
    }
    return -1;
}

static int chunk_idx_get(struct efs_export *ex, efs_ino_t ino, uint32_t chunk_index,
                         uint64_t *pos)
{
    if (!ex->chunk_keys || !ex->chunk_vals || !ex->chunks || ex->chunk_mask == 0)
        return -1;
    uint64_t key = hash_chunk_key(ino, chunk_index);
    uint64_t i = key & ex->chunk_mask;
    for (uint64_t n = 0; n <= ex->chunk_mask; n++) {
        if (ex->chunk_keys[i] == 0)
            return -1;
        if (ex->chunk_keys[i] == key) {
            uint64_t p = ex->chunk_vals[i];
            if (p < ex->chunk_count &&
                ex->chunks[p].ino == ino &&
                ex->chunks[p].chunk_index == chunk_index) {
                *pos = p;
                return 0;
            }
        }
        i = (i + 1) & ex->chunk_mask;
    }
    return -1;
}

/* --- per-ino live chunk count (ino -> count in this table) --- */
static uint32_t icnt_get(const struct efs_export *ex, efs_ino_t ino)
{
    if (!ex->icnt_keys || ex->icnt_mask == 0 || ino == 0)
        return 0;
    uint64_t i = idx_slot(ino, ex->icnt_mask);
    for (uint64_t n = 0; n <= ex->icnt_mask; n++) {
        if (ex->icnt_keys[i] == 0)
            return 0;
        if (ex->icnt_keys[i] == ino)
            return ex->icnt_vals[i];
        i = (i + 1) & ex->icnt_mask;
    }
    return 0;
}

static void icnt_put(struct efs_export *ex, efs_ino_t ino, uint32_t count)
{
    uint64_t i = idx_slot(ino, ex->icnt_mask);
    for (uint64_t n = 0; n <= ex->icnt_mask; n++) {
        if (ex->icnt_keys[i] == 0 || ex->icnt_keys[i] == ino) {
            ex->icnt_keys[i] = ino;
            ex->icnt_vals[i] = count;
            return;
        }
        i = (i + 1) & ex->icnt_mask;
    }
}

static void icnt_inc(struct efs_export *ex, efs_ino_t ino)
{
    if (!ex->icnt_keys || ex->icnt_mask == 0 || ino == 0)
        return;
    uint64_t i = idx_slot(ino, ex->icnt_mask);
    for (uint64_t n = 0; n <= ex->icnt_mask; n++) {
        if (ex->icnt_keys[i] == 0) {
            ex->icnt_keys[i] = ino;
            ex->icnt_vals[i] = 1;
            return;
        }
        if (ex->icnt_keys[i] == ino) {
            ex->icnt_vals[i]++;
            return;
        }
        i = (i + 1) & ex->icnt_mask;
    }
}

static void icnt_dec(struct efs_export *ex, efs_ino_t ino)
{
    if (!ex->icnt_keys || ex->icnt_mask == 0 || ino == 0)
        return;
    uint64_t i = idx_slot(ino, ex->icnt_mask);
    for (uint64_t n = 0; n <= ex->icnt_mask; n++) {
        if (ex->icnt_keys[i] == 0)
            return;
        if (ex->icnt_keys[i] == ino)
            break;
        i = (i + 1) & ex->icnt_mask;
    }
    if (ex->icnt_keys[i] != ino)
        return;
    if (ex->icnt_vals[i] > 1) {
        ex->icnt_vals[i]--;
        return;
    }
    /* count hits 0: delete with backfill (mirror chunk_idx_del). */
    ex->icnt_keys[i] = 0;
    ex->icnt_vals[i] = 0;
    uint64_t j = (i + 1) & ex->icnt_mask;
    while (ex->icnt_keys[j] != 0) {
        uint64_t k = ex->icnt_keys[j];
        uint32_t v = ex->icnt_vals[j];
        ex->icnt_keys[j] = 0;
        ex->icnt_vals[j] = 0;
        icnt_put(ex, k, v);
        j = (j + 1) & ex->icnt_mask;
    }
}

static int icnt_init(struct efs_export *ex, uint64_t n_hint)
{
    uint64_t cap = 16;
    while (cap < n_hint * 2)
        cap *= 2;
    free(ex->icnt_keys);
    free(ex->icnt_vals);
    ex->icnt_keys = calloc(cap, sizeof(uint64_t));
    ex->icnt_vals = calloc(cap, sizeof(uint32_t));
    if (!ex->icnt_keys || !ex->icnt_vals) {
        free(ex->icnt_keys);
        free(ex->icnt_vals);
        ex->icnt_keys = NULL;
        ex->icnt_vals = NULL;
        ex->icnt_mask = 0;
        return -1;
    }
    ex->icnt_mask = cap - 1;
    return 0;
}

static void icnt_free(struct efs_export *ex)
{
    free(ex->icnt_keys);
    free(ex->icnt_vals);
    ex->icnt_keys = NULL;
    ex->icnt_vals = NULL;
    ex->icnt_mask = 0;
}

static void chunk_idx_del(struct efs_export *ex, efs_ino_t ino, uint32_t chunk_index)
{
    if (!ex->chunk_keys || ex->chunk_mask == 0)
        return;
    uint64_t key = hash_chunk_key(ino, chunk_index);
    uint64_t i = key & ex->chunk_mask;
    for (uint64_t n = 0; n <= ex->chunk_mask; n++) {
        if (ex->chunk_keys[i] == 0)
            return;
        if (ex->chunk_keys[i] == key) {
            uint64_t p = ex->chunk_vals[i];
            if (p < ex->chunk_count &&
                ex->chunks[p].ino == ino &&
                ex->chunks[p].chunk_index == chunk_index)
                break;
        }
        i = (i + 1) & ex->chunk_mask;
    }
    if (ex->chunk_keys[i] != key)
        return;
    {
        uint64_t p = ex->chunk_vals[i];
        if (!(p < ex->chunk_count &&
              ex->chunks[p].ino == ino &&
              ex->chunks[p].chunk_index == chunk_index))
            return;
    }
    ex->chunk_keys[i] = 0;
    ex->chunk_vals[i] = 0;
    uint64_t j = (i + 1) & ex->chunk_mask;
    while (ex->chunk_keys[j] != 0) {
        uint64_t v = ex->chunk_vals[j];
        ex->chunk_keys[j] = 0;
        ex->chunk_vals[j] = 0;
        if (v < ex->chunk_count)
            chunk_idx_put(ex, ex->chunks[v].ino, ex->chunks[v].chunk_index, v);
        j = (j + 1) & ex->chunk_mask;
    }
}

/* Size hash tables for capacity, not live count, so a create burst does
 * not immediately rehash. */
static uint64_t inode_idx_hint(const struct efs_export *ex)
{
    uint64_t n = ex->inode_count ? ex->inode_count : 16;
    if (ex->inode_capacity > n)
        n = ex->inode_capacity;
    return n;
}

static uint64_t chunk_idx_hint(const struct efs_export *ex)
{
    uint64_t n = ex->chunk_count ? ex->chunk_count : 16;
    if (ex->chunk_capacity > n)
        n = ex->chunk_capacity;
    return n;
}

static int export_reindex_inodes(struct efs_export *ex)
{
    if (idx_init(&ex->ino_keys, &ex->ino_vals, &ex->ino_mask,
                 inode_idx_hint(ex)) != 0)
        return -1;
    if (idx_init(&ex->name_keys, &ex->name_vals, &ex->name_mask,
                 inode_idx_hint(ex)) != 0)
        return -1;
    for (uint64_t i = 0; i < ex->inode_count; i++) {
        if (inode_at(ex, i)->ino == 0)
            continue;
        idx_put(ex->ino_keys, ex->ino_vals, ex->ino_mask, inode_at(ex, i)->ino, i);
        name_idx_put(ex, inode_at(ex, i)->parent, efs_export_inode_name(ex, i), i);
    }
    return 0;
}

static int export_reindex_chunks(struct efs_export *ex)
{
    if (idx_init(&ex->chunk_keys, &ex->chunk_vals, &ex->chunk_mask,
                 chunk_idx_hint(ex)) != 0)
        return -1;
    /* Rebuild the per-ino chunk count alongside chunk_idx (same headroom). */
    if (icnt_init(ex, chunk_idx_hint(ex)) != 0)
        return -1;
    for (uint64_t i = 0; i < ex->chunk_count; i++) {
        chunk_idx_put(ex, ex->chunks[i].ino, ex->chunks[i].chunk_index, i);
        icnt_inc(ex, ex->chunks[i].ino);
    }
    return 0;
}

static int export_reindex(struct efs_export *ex)
{
    if (export_reindex_inodes(ex) != 0)
        return -1;
    return export_reindex_chunks(ex);
}

static int export_ensure_inode_idx(struct efs_export *ex)
{
    if (ex->ino_keys && ex->inode_count * 2 <= ex->ino_mask + 1)
        return 0;
    return export_reindex_inodes(ex);
}

static int export_ensure_chunk_idx(struct efs_export *ex)
{
    if (ex->chunk_keys && ex->chunk_count * 2 <= ex->chunk_mask + 1)
        return 0;
    return export_reindex_chunks(ex);
}

static struct efs_inode_mem *inode_ptr(struct efs_export *ex, efs_ino_t ino)
{
    uint64_t pos = 0;
    if (!ex)
        return NULL;
    if (ex->ino_keys) {
        struct efs_inode_mem *p;
        if (idx_get(ex->ino_keys, ex->ino_vals, ex->ino_mask, ino, &pos) == 0 &&
            pos < ex->inode_count && pos < ex->inode_capacity) {
            p = inode_at(ex, pos);
            if (p && p->ino == ino)
                return p;
        }
        return NULL;
    }
    {
        uint64_t n = ex->inode_count;
        uint64_t i;
        if (n > ex->inode_capacity)
            n = ex->inode_capacity;
        for (i = 0; i < n; i++) {
            struct efs_inode_mem *p = inode_at(ex, i);
            if (p && p->ino == ino)
                return p;
        }
    }
    return NULL;
}

/* ---- parent → children index (in-memory) ---- */

static void child_vecs_free(struct efs_export *ex)
{
    if (!ex)
        return;
    for (uint64_t i = 0; i < ex->child_vec_count; i++)
        free(ex->child_vecs[i].slots);
    free(ex->child_vecs);
    ex->child_vecs = NULL;
    ex->child_vec_count = 0;
    ex->child_vec_cap = 0;
    idx_free(&ex->child_keys, &ex->child_vals, &ex->child_mask);
}

/* idx_init() frees the destination arrays — detach first, rehash, then free. */
static int child_idx_rehash(struct efs_export *ex, uint64_t n_hint)
{
    uint64_t old_mask = ex->child_mask;
    uint64_t *ok = ex->child_keys;
    uint64_t *ov = ex->child_vals;
    ex->child_keys = NULL;
    ex->child_vals = NULL;
    ex->child_mask = 0;
    if (idx_init(&ex->child_keys, &ex->child_vals, &ex->child_mask, n_hint) != 0) {
        ex->child_keys = ok;
        ex->child_vals = ov;
        ex->child_mask = old_mask;
        return -1;
    }
    if (ok) {
        for (uint64_t i = 0; i <= old_mask; i++) {
            if (ok[i])
                idx_put(ex->child_keys, ex->child_vals, ex->child_mask, ok[i], ov[i]);
        }
        free(ok);
        free(ov);
    }
    return 0;
}

static struct efs_child_vec *child_vec_get(struct efs_export *ex, efs_ino_t parent,
                                          int create)
{
    if (!ex->child_keys || ex->child_mask == 0) {
        if (!create)
            return NULL;
        uint64_t hint = ex->inode_count ? ex->inode_count : 16;
        if (idx_init(&ex->child_keys, &ex->child_vals, &ex->child_mask, hint) != 0)
            return NULL;
    }
    uint64_t vi = 0;
    if (idx_get(ex->child_keys, ex->child_vals, ex->child_mask, parent, &vi) == 0 &&
        vi < ex->child_vec_count)
        return &ex->child_vecs[vi];
    if (!create)
        return NULL;
    if (ex->child_vec_count >= ex->child_vec_cap) {
        uint64_t ncap = ex->child_vec_cap ? ex->child_vec_cap * 2 : 16;
        struct efs_child_vec *n = realloc(ex->child_vecs,
                                          ncap * sizeof(struct efs_child_vec));
        if (!n)
            return NULL;
        if (ncap > ex->child_vec_cap)
            memset(n + ex->child_vec_cap, 0,
                   (ncap - ex->child_vec_cap) * sizeof(*n));
        ex->child_vecs = n;
        ex->child_vec_cap = ncap;
    }
    /* Grow open-addressing table if load is high.
     * idx_init() frees the key/val arrays — detach old pointers first so
     * we can rehash from them, then free once (avoid double-free). */
    if (ex->child_vec_count * 2 > ex->child_mask) {
        if (child_idx_rehash(ex, ex->child_vec_count + 1) != 0)
            return NULL;
    }
    vi = ex->child_vec_count++;
    memset(&ex->child_vecs[vi], 0, sizeof(ex->child_vecs[vi]));
    idx_put(ex->child_keys, ex->child_vals, ex->child_mask, parent, vi);
    return &ex->child_vecs[vi];
}

static int child_idx_add(struct efs_export *ex, efs_ino_t parent, uint64_t slot)
{
    if (parent == inode_at(ex, slot)->ino && parent == EFS_ROOT_INO)
        return 0; /* root is not a child of itself for listing */
    struct efs_child_vec *v = child_vec_get(ex, parent, 1);
    if (!v)
        return -1;
    if (v->count >= v->cap) {
        uint64_t ncap = v->cap ? v->cap * 2 : 4;
        uint64_t *ns = realloc(v->slots, ncap * sizeof(uint64_t));
        if (!ns)
            return -1;
        v->slots = ns;
        v->cap = ncap;
    }
    v->slots[v->count++] = slot;
    return 0;
}

static void child_idx_del(struct efs_export *ex, efs_ino_t parent, uint64_t slot)
{
    struct efs_child_vec *v = child_vec_get(ex, parent, 0);
    if (!v)
        return;
    for (uint64_t i = 0; i < v->count; i++) {
        if (v->slots[i] == slot) {
            v->slots[i] = v->slots[v->count - 1];
            v->count--;
            return;
        }
    }
}

static void child_idx_replace_slot(struct efs_export *ex, efs_ino_t parent,
                                   uint64_t old_slot, uint64_t new_slot)
{
    struct efs_child_vec *v = child_vec_get(ex, parent, 0);
    if (!v)
        return;
    for (uint64_t i = 0; i < v->count; i++) {
        if (v->slots[i] == old_slot) {
            v->slots[i] = new_slot;
            return;
        }
    }
}

static void child_idx_rebuild(struct efs_export *ex)
{
    child_vecs_free(ex);
    for (uint64_t i = 0; i < ex->inode_count; i++) {
        if (inode_at(ex, i)->ino == 0)
            continue;
        if (inode_at(ex, i)->ino == EFS_ROOT_INO &&
            inode_at(ex, i)->parent == EFS_ROOT_INO)
            continue;
        if (!efs_export_inode_name(ex, i)[0] || inode_at(ex, i)->parent == 0)
            continue;
        child_idx_add(ex, inode_at(ex, i)->parent, i);
    }
}

int efs_export_foreach_child(struct efs_export *ex, efs_ino_t parent,
                             efs_child_cb cb, void *arg)
{
    if (!ex || !cb)
        return EFS_ERR_INVAL;
    struct efs_child_vec *v = child_vec_get(ex, parent, 0);
    if (!v)
        return EFS_OK;
    for (uint64_t i = 0; i < v->count; i++) {
        uint64_t slot = v->slots[i];
        if (slot >= ex->inode_count)
            continue;
        /* ino==0 rows are not in the name index (lookup → ENOENT) but used
         * to appear in readdir — walkers then fstatat a listed name and fail. */
        if (inode_at(ex, slot)->ino == 0)
            continue;
        if (inode_at(ex, slot)->ino == parent)
            continue;
        if (inode_at(ex, slot)->parent != parent)
            continue;
        uint64_t npos = 0;
        if (name_idx_get(ex, parent, efs_export_inode_name(ex, slot), &npos) != 0 ||
            npos != slot)
            continue;
        int rc = cb(ex, slot, arg);
        if (rc != 0)
            return rc;
    }
    return EFS_OK;
}

static int dir_nonempty_cb(struct efs_export *ex, uint64_t slot, void *arg)
{
    (void)ex;
    (void)slot;
    (void)arg;
    return 1;  /* any child terminates the walk with a non-zero rc */
}

/* Return 1 if the directory has no children, 0 otherwise. */
int efs_export_dir_empty(struct efs_export *ex, efs_ino_t ino)
{
    return efs_export_foreach_child(ex, ino, dir_nonempty_cb, NULL) == 0;
}

/* ---- directory rollups ---- */

static uint64_t entry_tmin(const struct efs_inode_mem *e)
{
    uint64_t t = e->mtime;
    if (e->ctime < t)
        t = e->ctime;
    if (e->atime < t)
        t = e->atime;
    return t;
}

static uint64_t entry_tmax(const struct efs_inode_mem *e)
{
    uint64_t t = e->mtime;
    if (e->ctime > t)
        t = e->ctime;
    if (e->atime > t)
        t = e->atime;
    return t;
}

static void inode_clear_rollups(struct efs_inode_mem *d)
{
    d->imm_files = d->imm_dirs = d->tree_files = d->tree_dirs = 0;
    d->imm_bytes = d->tree_bytes = 0;
    d->imm_tmin = d->imm_tmax = d->tree_tmin = d->tree_tmax = 0;
}

static void times_expand(uint64_t *tmin, uint64_t *tmax, uint64_t lo, uint64_t hi,
                         int *has)
{
    if (!*has) {
        *tmin = lo;
        *tmax = hi;
        *has = 1;
        return;
    }
    if (lo < *tmin)
        *tmin = lo;
    if (hi > *tmax)
        *tmax = hi;
}

/* Child C's contribution to a parent's tree_* aggregates. */
static void child_tree_contrib(const struct efs_inode_mem *c, uint64_t *files,
                               uint64_t *dirs, uint64_t *bytes, uint64_t *tlo,
                               uint64_t *thi)
{
    *tlo = entry_tmin(c);
    *thi = entry_tmax(c);
    if (efs_mode_is_dir(c->mode)) {
        *files = c->tree_files;
        *dirs = 1 + c->tree_dirs;
        *bytes = c->tree_bytes;
        if (c->tree_files + c->tree_dirs > 0) {
            if (c->tree_tmin < *tlo)
                *tlo = c->tree_tmin;
            if (c->tree_tmax > *thi)
                *thi = c->tree_tmax;
        }
    } else {
        *files = 1;
        *dirs = 0;
        *bytes = c->size;
    }
}

static void parent_add_child(struct efs_inode_mem *p, const struct efs_inode_mem *c)
{
    uint64_t lo = entry_tmin(c), hi = entry_tmax(c);
    int has_imm = (p->imm_files + p->imm_dirs) > 0;
    int has_tree = (p->tree_files + p->tree_dirs) > 0;

    if (efs_mode_is_dir(c->mode)) {
        p->imm_dirs++;
        times_expand(&p->imm_tmin, &p->imm_tmax, lo, hi, &has_imm);

        uint64_t tf, td, tb, tlo, thi;
        child_tree_contrib(c, &tf, &td, &tb, &tlo, &thi);
        p->tree_files += tf;
        p->tree_dirs += td;
        p->tree_bytes += tb;
        times_expand(&p->tree_tmin, &p->tree_tmax, tlo, thi, &has_tree);
    } else {
        p->imm_files++;
        p->tree_files++;
        p->imm_bytes += c->size;
        p->tree_bytes += c->size;
        times_expand(&p->imm_tmin, &p->imm_tmax, lo, hi, &has_imm);
        times_expand(&p->tree_tmin, &p->tree_tmax, lo, hi, &has_tree);
    }
}

static void ancestor_add_tree(struct efs_inode_mem *a, uint64_t files, uint64_t dirs,
                              uint64_t bytes, uint64_t tlo, uint64_t thi)
{
    int has = (a->tree_files + a->tree_dirs) > 0;
    a->tree_files += files;
    a->tree_dirs += dirs;
    a->tree_bytes += bytes;
    times_expand(&a->tree_tmin, &a->tree_tmax, tlo, thi, &has);
}

static void ancestor_sub_tree(struct efs_inode_mem *a, uint64_t files, uint64_t dirs,
                              uint64_t bytes)
{
    if (a->tree_files >= files)
        a->tree_files -= files;
    else
        a->tree_files = 0;
    if (a->tree_dirs >= dirs)
        a->tree_dirs -= dirs;
    else
        a->tree_dirs = 0;
    if (a->tree_bytes >= bytes)
        a->tree_bytes -= bytes;
    else
        a->tree_bytes = 0;
}

static void parent_sub_child_counts(struct efs_inode_mem *p, const struct efs_inode_mem *c)
{
    if (efs_mode_is_dir(c->mode)) {
        if (p->imm_dirs > 0)
            p->imm_dirs--;
        uint64_t tf, td, tb, tlo, thi;
        child_tree_contrib(c, &tf, &td, &tb, &tlo, &thi);
        ancestor_sub_tree(p, tf, td, tb);
    } else {
        if (p->imm_files > 0)
            p->imm_files--;
        if (p->tree_files > 0)
            p->tree_files--;
        if (p->imm_bytes >= c->size)
            p->imm_bytes -= c->size;
        else
            p->imm_bytes = 0;
        if (p->tree_bytes >= c->size)
            p->tree_bytes -= c->size;
        else
            p->tree_bytes = 0;
    }
    if (p->imm_files + p->imm_dirs == 0) {
        p->imm_tmin = p->imm_tmax = 0;
        p->imm_bytes = 0;
    }
    if (p->tree_files + p->tree_dirs == 0) {
        p->tree_tmin = p->tree_tmax = 0;
        p->tree_bytes = 0;
    }
}

static void recompute_times_one(struct efs_export *ex, efs_ino_t dir_ino)
{
    struct efs_inode_mem *d = inode_ptr(ex, dir_ino);
    if (!d || !efs_mode_is_dir(d->mode))
        return;
    d->imm_tmin = d->imm_tmax = d->tree_tmin = d->tree_tmax = 0;
    int has_imm = 0, has_tree = 0;
    struct efs_child_vec *v = child_vec_get(ex, dir_ino, 0);
    if (!v)
        return;
    for (uint64_t i = 0; i < v->count; i++) {
        uint64_t slot = v->slots[i];
        if (slot >= ex->inode_count)
            continue;
        struct efs_inode_mem *c = inode_at(ex, slot);
        if (c->ino == dir_ino)
            continue;
        uint64_t lo = entry_tmin(c), hi = entry_tmax(c);
        times_expand(&d->imm_tmin, &d->imm_tmax, lo, hi, &has_imm);
        uint64_t tf, td, tb, tlo, thi;
        child_tree_contrib(c, &tf, &td, &tb, &tlo, &thi);
        (void)tf;
        (void)td;
        (void)tb;
        times_expand(&d->tree_tmin, &d->tree_tmax, tlo, thi, &has_tree);
    }
}

static void recompute_times_up(struct efs_export *ex, efs_ino_t dir_ino)
{
    efs_ino_t cur = dir_ino;
    int hops = 0;
    for (;;) {
        recompute_times_one(ex, cur);
        struct efs_inode_mem *d = inode_ptr(ex, cur);
        if (!d || d->parent == d->ino || d->parent == 0 || hops++ >= 128)
            break;
        cur = d->parent;
    }
}

static void rollup_add_under(struct efs_export *ex, efs_ino_t parent,
                             const struct efs_inode_mem *child)
{
    struct efs_inode_mem *p = inode_ptr(ex, parent);
    if (!p || !efs_mode_is_dir(p->mode))
        return;
    parent_add_child(p, child);

    uint64_t tf, td, tb, tlo, thi;
    child_tree_contrib(child, &tf, &td, &tb, &tlo, &thi);
    efs_ino_t a = p->parent;
    efs_ino_t prev = parent;
    int hops = 0;
    while (a != 0 && a != prev && hops++ < 128) {
        struct efs_inode_mem *ap = inode_ptr(ex, a);
        if (!ap || ap->ino != a || !efs_mode_is_dir(ap->mode))
            break;
        ancestor_add_tree(ap, tf, td, tb, tlo, thi);
        if (ap->parent == a)
            break;
        prev = a;
        a = ap->parent;
    }
}

static void rollup_sub_under(struct efs_export *ex, efs_ino_t parent,
                             const struct efs_inode_mem *child)
{
    struct efs_inode_mem *p = inode_ptr(ex, parent);
    if (!p || !efs_mode_is_dir(p->mode))
        return;

    uint64_t tf, td, tb, tlo, thi;
    child_tree_contrib(child, &tf, &td, &tb, &tlo, &thi);
    parent_sub_child_counts(p, child);

    efs_ino_t a = p->parent;
    efs_ino_t prev = parent;
    int hops = 0;
    while (a != 0 && a != prev && hops++ < 128) {
        struct efs_inode_mem *ap = inode_ptr(ex, a);
        if (!ap || ap->ino != a || !efs_mode_is_dir(ap->mode))
            break;
        ancestor_sub_tree(ap, tf, td, tb);
        if (ap->parent == a)
            break;
        prev = a;
        a = ap->parent;
    }
    recompute_times_up(ex, parent);
}

static void rollup_size_delta(struct efs_export *ex, efs_ino_t parent,
                              int64_t delta)
{
    if (delta == 0)
        return;
    efs_ino_t cur = parent;
    efs_ino_t prev = 0;
    int first = 1;
    int hops = 0;
    while (cur != 0 && cur != prev && hops++ < 128) {
        struct efs_inode_mem *d = inode_ptr(ex, cur);
        if (!d || d->ino != cur || !efs_mode_is_dir(d->mode))
            break;
        if (first) {
            if (delta > 0)
                d->imm_bytes += (uint64_t)delta;
            else if (d->imm_bytes >= (uint64_t)(-delta))
                d->imm_bytes -= (uint64_t)(-delta);
            else
                d->imm_bytes = 0;
            first = 0;
        }
        if (delta > 0)
            d->tree_bytes += (uint64_t)delta;
        else if (d->tree_bytes >= (uint64_t)(-delta))
            d->tree_bytes -= (uint64_t)(-delta);
        else
            d->tree_bytes = 0;
        if (d->parent == cur)
            break;
        prev = cur;
        cur = d->parent;
    }
}

/* Incremental time rollup for "time moved forward" mutations (create/write/
 * chmod/chown/utimens all stamp ctime = now, and entry_tmax is dominated by
 * ctime). The child's entry range can only widen an ancestor's [tmin,tmax],
 * never narrow it, so expand in O(depth) instead of the O(children) rescan in
 * recompute_times_up. Without this, every write in a growing directory cost
 * O(dir_children) → O(n^2) bulk copies. The narrowing case (unlink) still does
 * a full rescan via rollup_sub_under → recompute_times_up. */
static void expand_parent_chain(struct efs_export *ex, efs_ino_t from_ino,
                                efs_ino_t parent, uint64_t lo, uint64_t hi)
{
    efs_ino_t cur = parent;
    efs_ino_t prev = from_ino;
    int first = 1;
    int hops = 0;
    while (cur != 0 && cur != prev && hops++ < 128) {
        struct efs_inode_mem *d = inode_ptr(ex, cur);
        if (!d || d->ino != cur || !efs_mode_is_dir(d->mode))
            break;
        if (first) { /* immediate parent also tracks imm_* */
            int has = (d->imm_files + d->imm_dirs) > 0;
            times_expand(&d->imm_tmin, &d->imm_tmax, lo, hi, &has);
            first = 0;
        }
        int hast = (d->tree_files + d->tree_dirs) > 0;
        times_expand(&d->tree_tmin, &d->tree_tmax, lo, hi, &hast);
        if (d->parent == cur)
            break;
        prev = cur;
        cur = d->parent;
    }
}

static void rollup_expand_parents_of(struct efs_export *ex, efs_ino_t ino)
{
    struct efs_inode_mem *p = inode_ptr(ex, ino);
    if (!p)
        return;
    uint64_t lo = entry_tmin(p), hi = entry_tmax(p);
    /* Directories always have nlink>=2 (".", subdirs) but a single inode row —
     * not hardlinks. Take the O(depth) single-parent walk for them; only scan
     * all rows for genuinely hardlinked multi-link files (rare). */
    if (p->nlink <= 1 || efs_mode_is_dir(p->mode)) {
        expand_parent_chain(ex, ino, p->parent, lo, hi);
        return;
    }
    /* Hardlinked: one inode row per link, each with its own parent (rare). */
    for (uint64_t i = 0; i < ex->inode_count; i++) {
        if (inode_at(ex, i)->ino == ino)
            expand_parent_chain(ex, ino, inode_at(ex, i)->parent, lo, hi);
    }
}

static void recompute_dir_postorder(struct efs_export *ex, efs_ino_t dir_ino,
                                    uint8_t *vis)
{
    struct efs_inode_mem *d = inode_ptr(ex, dir_ino);
    if (!d || !efs_mode_is_dir(d->mode))
        return;
    /* A torn/holed meta page can scramble parent pointers into a directory
     * cycle (a dir that is its own ancestor). Recursing on it overflowed the
     * catch-up thread's stack — the "raced deserialize SIGSEGV". Mark visited
     * dir slots and bail on a revisit so a garbage table cannot recurse
     * forever; the cycle's rollups stay stale until a clean flush re-publishes
     * the subtree. */
    size_t dslot = (size_t)inode_slot_of(ex, d);
    if (dslot < ex->inode_count) {
        if (vis[dslot])
            return;
        vis[dslot] = 1;
    }
    inode_clear_rollups(d);
    struct efs_child_vec *v = child_vec_get(ex, dir_ino, 0);
    if (!v)
        return;
    for (uint64_t i = 0; i < v->count; i++) {
        uint64_t slot = v->slots[i];
        if (slot >= ex->inode_count)
            continue;
        struct efs_inode_mem *c = inode_at(ex, slot);
        if (c->ino == dir_ino)
            continue;
        if (efs_mode_is_dir(c->mode))
            recompute_dir_postorder(ex, c->ino, vis);
        parent_add_child(d, c);
    }
}

static void pending_rollup_clear(struct efs_export *ex)
{
    ex->pending_rollup_count = 0;
    ex->rollups_stale = 0;
}

static int pending_rollup_note(struct efs_export *ex, efs_ino_t ino,
                               int64_t size_delta, int touch)
{
    for (uint64_t i = 0; i < ex->pending_rollup_count; i++) {
        if (ex->pending_rollup_inos[i] == ino) {
            ex->pending_rollup_deltas[i] += size_delta;
            if (touch)
                ex->pending_rollup_touch[i] = 1;
            return 0;
        }
    }
    if (ex->pending_rollup_count >= ex->pending_rollup_cap) {
        uint64_t ncap = ex->pending_rollup_cap ? ex->pending_rollup_cap * 2 : 64;
        efs_ino_t *ninos = realloc(ex->pending_rollup_inos, ncap * sizeof(*ninos));
        int64_t *ndeltas = realloc(ex->pending_rollup_deltas, ncap * sizeof(*ndeltas));
        uint8_t *ntouch = realloc(ex->pending_rollup_touch, ncap * sizeof(*ntouch));
        if (!ninos || !ndeltas || !ntouch) {
            free(ninos);
            free(ndeltas);
            free(ntouch);
            return -1;
        }
        ex->pending_rollup_inos = ninos;
        ex->pending_rollup_deltas = ndeltas;
        ex->pending_rollup_touch = ntouch;
        ex->pending_rollup_cap = ncap;
    }
    uint64_t i = ex->pending_rollup_count++;
    ex->pending_rollup_inos[i] = ino;
    ex->pending_rollup_deltas[i] = size_delta;
    ex->pending_rollup_touch[i] = touch ? 1 : 0;
    return 0;
}

static void apply_size_delta_for_ino(struct efs_export *ex, efs_ino_t ino,
                                     int64_t delta)
{
    if (delta == 0)
        return;
    struct efs_inode_mem *p = inode_ptr(ex, ino);
    if (!p || efs_mode_is_dir(p->mode))
        return;
    if (p->nlink <= 1) {
        rollup_size_delta(ex, p->parent, delta);
        return;
    }
    for (uint64_t i = 0; i < ex->inode_count; i++) {
        if (inode_at(ex, i)->ino == ino)
            rollup_size_delta(ex, inode_at(ex, i)->parent, delta);
    }
}

void efs_export_recompute_rollups(struct efs_export *ex)
{
    if (!ex)
        return;
    for (uint64_t i = 0; i < ex->inode_count; i++) {
        if (efs_mode_is_dir(inode_at(ex, i)->mode))
            inode_clear_rollups(inode_at(ex, i));
        else
            inode_clear_rollups(inode_at(ex, i));
    }
    child_idx_rebuild(ex);
    /* Visited-set for cycle-safe postorder (see recompute_dir_postorder). On
     * allocation failure skip the recompute rather than risk the recursion —
     * rollups stay cleared (stale) instead of crashing the catch-up thread. */
    uint8_t *vis = calloc(ex->inode_count ? ex->inode_count : 1, 1);
    if (vis) {
        recompute_dir_postorder(ex, EFS_ROOT_INO, vis);
        free(vis);
    }
    pending_rollup_clear(ex);
}

void efs_export_ensure_rollups(struct efs_export *ex)
{
    if (!ex || !ex->rollups_stale)
        return;
    for (uint64_t i = 0; i < ex->pending_rollup_count; i++) {
        efs_ino_t ino = ex->pending_rollup_inos[i];
        int64_t delta = ex->pending_rollup_deltas[i];
        uint8_t flags = ex->pending_rollup_touch[i];
        if (flags & EFS_ROLLUP_CREATE) {
            struct efs_inode_mem *c = inode_ptr(ex, ino);
            if (c)
                rollup_add_under(ex, c->parent, c);
            if (delta != 0)
                apply_size_delta_for_ino(ex, ino, delta);
        } else {
            apply_size_delta_for_ino(ex, ino, delta);
            if (delta != 0 || (flags & EFS_ROLLUP_TOUCH))
                rollup_expand_parents_of(ex, ino);
        }
    }
    pending_rollup_clear(ex);
}

int efs_export_format_stats_ex(const struct efs_export *ex,
                               const struct efs_inode *dir, char *buf,
                               size_t buflen)
{
    if (!dir || !buf || buflen == 0 || !efs_mode_is_dir(dir->mode))
        return -1;
    uint32_t ino_pg = 0, ch_pg = 0;
    if (ex)
        efs_export_meta_page_usage(ex, &ino_pg, &ch_pg);
    int n = snprintf(buf, buflen,
                     "imm_files=%llu\n"
                     "imm_dirs=%llu\n"
                     "imm_bytes=%llu\n"
                     "imm_tmin=%llu\n"
                     "imm_tmax=%llu\n"
                     "tree_files=%llu\n"
                     "tree_dirs=%llu\n"
                     "tree_bytes=%llu\n"
                     "tree_tmin=%llu\n"
                     "tree_tmax=%llu\n"
                     "meta_ino_pages=%u\n"
                     "meta_ino_pages_max=%u\n"
                     "meta_chunk_pages=%u\n"
                     "meta_chunk_pages_max=%u\n",
                     (unsigned long long)dir->imm_files,
                     (unsigned long long)dir->imm_dirs,
                     (unsigned long long)dir->imm_bytes,
                     (unsigned long long)dir->imm_tmin,
                     (unsigned long long)dir->imm_tmax,
                     (unsigned long long)dir->tree_files,
                     (unsigned long long)dir->tree_dirs,
                     (unsigned long long)dir->tree_bytes,
                     (unsigned long long)dir->tree_tmin,
                     (unsigned long long)dir->tree_tmax,
                     ino_pg, (unsigned)EFS_META_INO_PAGE_MAX,
                     ch_pg, (unsigned)EFS_META_CHUNK_PAGE_MAX);
    if (n < 0 || (size_t)n >= buflen)
        return -1;
    return n;
}

/* Copy shared inode fields onto every hard-link row with the same ino.
 * Parent/name of each directory entry are preserved. */
static void sync_hardlink_attrs(struct efs_export *ex, efs_ino_t ino,
                                const struct efs_inode_mem *src)
{
    /* Single link: attrs already live on the indexed row (inode_ptr).
     * Directories have nlink>=2 without hardlink rows — nothing to sync. */
    if (!src || src->nlink <= 1 || efs_mode_is_dir(src->mode))
        return;
    for (uint64_t i = 0; i < ex->inode_count; i++) {
        if (inode_at(ex, i)->ino != ino)
            continue;
        efs_ino_t parent = inode_at(ex, i)->parent;
        uint32_t noff = inode_at(ex, i)->name_off;
        uint16_t nlen = inode_at(ex, i)->name_len;
        uint16_t nslab = inode_at(ex, i)->slab_idx;
        /* Preserve per-dent identity; shared attrs include atime/size/times.
         * Rollups stay zero on file dents. */
        uint64_t imm_files = inode_at(ex, i)->imm_files;
        uint64_t imm_dirs = inode_at(ex, i)->imm_dirs;
        uint64_t tree_files = inode_at(ex, i)->tree_files;
        uint64_t tree_dirs = inode_at(ex, i)->tree_dirs;
        uint64_t imm_bytes = inode_at(ex, i)->imm_bytes;
        uint64_t tree_bytes = inode_at(ex, i)->tree_bytes;
        uint64_t imm_tmin = inode_at(ex, i)->imm_tmin;
        uint64_t imm_tmax = inode_at(ex, i)->imm_tmax;
        uint64_t tree_tmin = inode_at(ex, i)->tree_tmin;
        uint64_t tree_tmax = inode_at(ex, i)->tree_tmax;
        (*inode_at(ex, i)) = *src;
        inode_at(ex, i)->parent = parent;
        inode_at(ex, i)->name_off = noff;
        inode_at(ex, i)->name_len = nlen;
        inode_at(ex, i)->slab_idx = nslab;
        inode_at(ex, i)->imm_files = imm_files;
        inode_at(ex, i)->imm_dirs = imm_dirs;
        inode_at(ex, i)->tree_files = tree_files;
        inode_at(ex, i)->tree_dirs = tree_dirs;
        inode_at(ex, i)->imm_bytes = imm_bytes;
        inode_at(ex, i)->tree_bytes = tree_bytes;
        inode_at(ex, i)->imm_tmin = imm_tmin;
        inode_at(ex, i)->imm_tmax = imm_tmax;
        inode_at(ex, i)->tree_tmin = tree_tmin;
        inode_at(ex, i)->tree_tmax = tree_tmax;
    }
}

/* Swap-remove chunks[j]; caller already bumped layout_epoch. */
static void remove_chunk_at(struct efs_export *ex, uint64_t j)
{
    ex->chunk_epoch++;
    uint32_t cidx = ex->chunks[j].chunk_index;
    efs_ino_t ino = ex->chunks[j].ino;
    chunk_idx_del(ex, ino, cidx);
    icnt_dec(ex, ino);
    uint64_t clast = ex->chunk_count - 1;
    if (j != clast) {
        chunk_idx_del(ex, ex->chunks[clast].ino, ex->chunks[clast].chunk_index);
        ex->chunks[j] = ex->chunks[clast];
        ex->chunk_count--;
        chunk_idx_put(ex, ex->chunks[j].ino, ex->chunks[j].chunk_index, j);
    } else {
        ex->chunk_count--;
    }
}

/* Scan this table's chunk array. After extent sharding a single table
 * only holds some groups, so a sequential ci walk stops at the first
 * hole and leaks the rest. */
static void drop_chunks_scan(struct efs_export *ex, efs_ino_t ino,
                             uint32_t first_chunk)
{
    /* efs_export_drop_chunks_from fans this over the main table AND every
     * loaded shard tab, but an ino's chunks live in only one or a few of them.
     * For every other table icnt_get() is 0, the fast path below is skipped
     * (it requires total > 0) and the fallback scans that unrelated table's
     * entire chunk array -- so one unlink cost O(all chunks on this node).
     * That is the term behind unlink p50 growing 0.44ms -> 4.6ms between 100k
     * and 5M inodes. icnt is maintained everywhere chunk_idx is (set_chunk,
     * merge, remove_chunk_at, export_reindex_chunks), so a zero is exactly as
     * trustworthy as the count the fast path already returns on. Checked
     * before layout_epoch++: a table with nothing to drop did not change, and
     * a spurious bump makes the flush treat the table as raced and stay dirty.
     * Holds for first_chunk > 0 too -- no chunks means none in any range. */
    if (ex->icnt_keys && ex->icnt_mask != 0 && icnt_get(ex, ino) == 0)
        return;
    ex->layout_epoch++;
    /* Fast path: probe by chunk_index via chunk_idx, bounded by the per-ino
     * live count, instead of scanning the whole chunk array under the
     * metadata lock. Only valid for a full drop (first_chunk==0): the count
     * is the exact number of this ino's chunks in this table, so finding
     * `total` of them means none are left. A partial truncate (first_chunk>0)
     * has no exact per-range count, and a missing or stale count falls
     * through to the authoritative scan below. */
    uint64_t removed = 0;
    if (first_chunk == 0) {
        uint32_t total = icnt_get(ex, ino);
        if (total > 0) {
            uint32_t found = 0;
            /* Generous bound: a dense file needs `total` probes, a sparse one
             * more. On exhaustion (stale/inflated count or extreme layout)
             * fall through to the scan. */
            uint64_t limit = (uint64_t)total * 4 + 4096;
            for (uint64_t ci = 0; found < total && ci < limit; ci++) {
                uint64_t pos = 0;
                if (chunk_idx_get(ex, ino, (uint32_t)ci, &pos) == 0) {
                    remove_chunk_at(ex, pos);
                    found++;
                }
            }
            removed += found;
            if (found == total) {
                if (removed) {
                    ex->shard_dirty = 1;
                }
                return;
            }
        }
    }
    uint64_t i = 0;
    while (i < ex->chunk_count) {
        if (ex->chunks[i].ino == ino &&
            ex->chunks[i].chunk_index >= first_chunk) {
            remove_chunk_at(ex, i);
            removed++;
        } else {
            i++;
        }
    }
    /* Mark dirty HERE, per table, and only when this table actually lost a
     * chunk. The DROP_CHUNKS handler used to set ex->shard_dirty on the MAIN
     * table unconditionally for every fanned drop, so one unlink anywhere
     * latched every peer's main table dirty forever — nothing on a peer ever
     * clears it, because only a main-table flush does and peers do not own
     * shard 0. That permanently tripped the META_COMMIT promote gate
     * (`if (ex->shard_dirty) reply = BUSY`), which is why peers stashed every
     * prepare but stayed pinned at an old committed gen. */
    if (removed) {
        ex->shard_dirty = 1;
    }
}

static void remove_chunks_for_ino(struct efs_export *ex, efs_ino_t ino)
{
    efs_export_drop_chunks_from(ex, ino, 0);
}

/* Remove every row for ino from ONE table (no rollup/nlink bookkeeping —
 * this is cache eviction, not an unlink). A create dual-apply stages a
 * dentry stub on the parent's tab plus the full row on the ino's tab, and
 * hardlinks add one row per link, so loop until the ino index no longer
 * maps the ino here. */
static void forget_ino_on_tab(struct efs_export *ex, efs_ino_t ino)
{
    if (!ex || !ex->ino_keys)
        return;
    for (;;) {
        uint64_t pos = 0;
        if (efs_export_inode_slot(ex, ino, &pos) != EFS_OK)
            break;
        if (pos >= ex->inode_count)
            break;
        struct efs_inode_mem *row = inode_at(ex, pos);
        if (!row || row->ino != ino)
            break;
        /* nlink > 1 hints another link row may share this ino on this tab;
         * remove_inode_slot then re-indexes a survivor (or drops the key).
         * nlink <= 1 takes the O(1) idx_del path. */
        int expect_survivor = row->nlink > 1;
        child_idx_del(ex, row->parent, pos);
        remove_inode_slot(ex, pos, expect_survivor);
    }
}

void efs_export_forget_ino(struct efs_export *ex, efs_ino_t ino)
{
    if (!ex || !ino)
        return;
    /* Chunk recs first: drop_chunks_scan fans across the main table and
     * every loaded shard tab with an icnt fast-path miss. */
    efs_export_drop_chunks_from(ex, ino, 0);
    forget_ino_on_tab(ex, ino);
    if (export_is_sharded_root(ex) && ex->shard_tabs) {
        for (uint32_t s = 1; s < ex->shard_tab_cap; s++) {
            if (ex->shard_tabs[s])
                forget_ino_on_tab(ex->shard_tabs[s], ino);
        }
    }
}

void efs_export_drop_chunks_from(struct efs_export *ex, efs_ino_t ino,
                                 uint32_t first_chunk)
{
    if (!ex)
        return;
    if (export_is_sharded_root(ex) && ex->root.shard_count > 1) {
        drop_chunks_scan(ex, ino, first_chunk);
        if (ex->shard_tabs) {
            for (uint32_t s = 1; s < ex->shard_tab_cap; s++) {
                if (ex->shard_tabs[s])
                    drop_chunks_scan(ex->shard_tabs[s], ino, first_chunk);
            }
        }
        return;
    }
    drop_chunks_scan(ex, ino, first_chunk);
}

/* Swap-remove inode array slot i; refresh indexes for the moved row.
 * Caller must already have applied rollup_sub and child_idx_del for slot i.
 * expect_survivor: another hard-link row for this ino remains (nlink > 0). */
static void remove_inode_slot(struct efs_export *ex, uint64_t i, int expect_survivor)
{
    efs_ino_t old_parent = inode_at(ex, i)->parent;
    char old_name[EFS_MAX_NAME];
    memset(old_name, 0, sizeof(old_name));
    strncpy(old_name, efs_export_inode_name(ex, i), EFS_MAX_NAME - 1);
    efs_ino_t old_ino = inode_at(ex, i)->ino;

    name_idx_del(ex, old_parent, old_name);
    dentry_bytes_sub(ex, old_name);

    ex->layout_epoch++;
    uint64_t last = ex->inode_count - 1;
    if (i != last) {
        efs_ino_t moved_parent = inode_at(ex, last)->parent;
        idx_del(ex->ino_keys, ex->ino_vals, ex->ino_mask, inode_at(ex, last)->ino);
        name_idx_del(ex, inode_at(ex, last)->parent, efs_export_inode_name(ex, last));
        (void)inode_row_move(ex, i, last);
        ex->inode_count--;
        idx_put(ex->ino_keys, ex->ino_vals, ex->ino_mask, inode_at(ex, i)->ino, i);
        name_idx_put(ex, inode_at(ex, i)->parent, efs_export_inode_name(ex, i), i);
        child_idx_replace_slot(ex, moved_parent, last, i);
    } else {
        ex->inode_count--;
    }

    /* ino_idx holds one slot per ino. If it still points at a live row with
     * old_ino (hard-link survivor, or the moved last row was the same ino),
     * leave it. Otherwise drop it — do not scan the inode table. */
    uint64_t pos = 0;
    if (ex->ino_keys &&
        idx_get(ex->ino_keys, ex->ino_vals, ex->ino_mask, old_ino, &pos) == 0 &&
        pos < ex->inode_count && inode_at(ex, pos)->ino == old_ino)
        return;

    if (!expect_survivor) {
        idx_del(ex->ino_keys, ex->ino_vals, ex->ino_mask, old_ino);
        return;
    }

    /* Rare: unlinked the indexed hard-link row; find another dent. */
    uint64_t survivor = UINT64_MAX;
    for (uint64_t k = 0; k < ex->inode_count; k++) {
        if (inode_at(ex, k)->ino == old_ino) {
            survivor = k;
            break;
        }
    }
    if (survivor != UINT64_MAX)
        idx_put(ex->ino_keys, ex->ino_vals, ex->ino_mask, old_ino, survivor);
    else
        idx_del(ex->ino_keys, ex->ino_vals, ex->ino_mask, old_ino);
}

void efs_export_init(struct efs_export *ex, efs_export_id_t id, const char *name)
{
    memset(ex, 0, sizeof(*ex));
    ex->id = id;
    strncpy(ex->name, name, EFS_MAX_NAME - 1);
    ex->chunk_size = EFS_DEFAULT_CHUNK_SIZE;
    ex->features = EFS_FEATURES_DEFAULT;
    ex->next_ino = EFS_ROOT_INO + 1;

    if (inode_ensure_cap(ex, 16) != EFS_OK)
        return;

    ex->chunk_capacity = 16;
    ex->chunks = calloc(ex->chunk_capacity, sizeof(struct efs_chunk_entry));

    struct efs_inode root = {0};
    root.ino = EFS_ROOT_INO;
    root.parent = EFS_ROOT_INO;
    root.mode = S_IFDIR | 0755;
    root.nlink = 2;
    strcpy(root.name, "/");
    time_now(&root.mtime);
    root.ctime = root.mtime;
    root.atime = root.mtime;
    inode_from_rpc(ex, inode_at(ex, ex->inode_count++), &root);
    dentry_bytes_add(ex, root.name);
    export_reindex(ex);
    child_idx_rebuild(ex);
    ex->efsm_version = EFS_META_EFSM_V6;
    ex->root.shard_count = 1;
    ex->root.shard_bits = 0;
}

void efs_export_free(struct efs_export *ex)
{
    if (!ex)
        return;
    if (ex->shard_tabs) {
        for (uint32_t i = 0; i < ex->shard_tab_cap; i++) {
            if (!ex->shard_tabs[i])
                continue;
            efs_export_free(ex->shard_tabs[i]);
            free(ex->shard_tabs[i]);
        }
        free(ex->shard_tabs);
        ex->shard_tabs = NULL;
        ex->shard_tab_cap = 0;
    }
    free(ex->inodes);
    if (ex->ino_slabs) {
        uint32_t i;
        for (i = 0; i < ex->ino_slab_n; i++) {
            free(ex->ino_slabs[i].rows);
            free(ex->ino_slabs[i].names);
        }
        free(ex->ino_slabs);
        ex->ino_slabs = NULL;
    }
    free(ex->chunks);
    free(ex->pending_rollup_inos);
    free(ex->pending_rollup_deltas);
    free(ex->pending_rollup_touch);
    idx_free(&ex->ino_keys, &ex->ino_vals, &ex->ino_mask);
    idx_free(&ex->name_keys, &ex->name_vals, &ex->name_mask);
    idx_free(&ex->chunk_keys, &ex->chunk_vals, &ex->chunk_mask);
    icnt_free(ex);
    child_vecs_free(ex);
    efs_export_root_free(&ex->root);
    free(ex->gm_blob);
    free(ex->flush_blob);
    memset(ex, 0, sizeof(*ex));
}

static void efs_export_init_empty_table(struct efs_export *ex, efs_export_id_t id,
                                        const char *name, uint32_t shard_bits,
                                        uint32_t shard_count, uint32_t shard)
{
    memset(ex, 0, sizeof(*ex));
    ex->id = id;
    if (name)
        strncpy(ex->name, name, EFS_MAX_NAME - 1);
    ex->chunk_size = EFS_DEFAULT_CHUNK_SIZE;
    ex->features = EFS_FEATURES_DEFAULT;
    if (inode_ensure_cap(ex, 16) != EFS_OK)
        return;
    ex->chunk_capacity = 16;
    ex->chunks = calloc(ex->chunk_capacity, sizeof(struct efs_chunk_entry));
    ex->efsm_version = EFS_META_VERSION;
    ex->root.shard_count = shard_count;
    ex->root.shard_bits = shard_bits;
    if (shard_bits)
        ex->next_ino = shard; /* low-bits: this shard's congruence-class base */
    export_reindex(ex);
    child_idx_rebuild(ex);
}

/* Get or lazily create the shard table for `shard`: grow the shard_tabs
 * array, calloc + init the table, and reload a previously flushed shard root
 * from the cluster root's extra_roots (this is the on-demand shard load).
 * This is a PER-EXPORT mutation — call only under the global lock or while
 * holding all the export's shard locks (rebuild). Blocker 2 pre-creates every
 * table at rehash/rebuild so op/read paths never reach this under a single
 * shard lock. */
static struct efs_export *shard_tab_get_or_create(struct efs_export *ex,
                                                  uint32_t shard)
{
    uint32_t sc = ex->root.shard_count ? ex->root.shard_count : 1;
    if (!ex->shard_tabs || ex->shard_tab_cap < sc) {
        struct efs_export **n = calloc(sc, sizeof(*n));
        if (!n)
            return NULL;
        if (ex->shard_tabs) {
            uint32_t old = ex->shard_tab_cap < sc ? ex->shard_tab_cap : sc;
            memcpy(n, ex->shard_tabs, (size_t)old * sizeof(*n));
            free(ex->shard_tabs);
        }
        ex->shard_tabs = n;
        ex->shard_tab_cap = sc;
    }
    if (!ex->shard_tabs[shard]) {
        struct efs_export *tab = calloc(1, sizeof(*tab));
        if (!tab)
            return NULL;
        efs_export_init_empty_table(tab, ex->id, ex->name, ex->root.shard_bits,
                                    sc, shard);
        tab->chunk_size = ex->chunk_size;
        tab->features = ex->features;
        tab->shard_id = shard;
        /* Reload a previously flushed extra from the cluster root. */
        for (uint32_t i = 0; i < ex->root.extra_shard_count; i++) {
            if (ex->root.extra_shard_ids && ex->root.extra_shard_ids[i] == shard &&
                ex->root.extra_roots) {
                if (efs_export_root_copy(&tab->root, &ex->root.extra_roots[i]) ==
                    EFS_OK) {
                    tab->meta_fragmented = 1;
                    tab->meta_needs_rebuild = (tab->root.page_count > 0);
                    tab->next_ino = tab->root.next_ino;
                }
                break;
            }
        }
        ex->shard_tabs[shard] = tab;
    }
    return ex->shard_tabs[shard];
}

struct efs_export *efs_export_table(struct efs_export *ex, uint32_t shard)
{
    if (!ex)
        return NULL;
    uint32_t sc = ex->root.shard_count ? ex->root.shard_count : 1;
    if (shard == 0 || ex->root.shard_bits == 0 || sc <= 1)
        return ex;
    if (shard >= sc || shard >= EFS_META_MAX_SHARDS)
        return NULL;
    struct efs_export *tab = shard_tab_get_or_create(ex, shard);
    if (!tab)
        return NULL;
    /* LRU tick. Atomic so a (future) shard-lock-holding read path can bump it
     * without the global lock; the value only guides cold-shard eviction. */
    tab->shard_tick = __atomic_add_fetch(&ex->shard_tick, 1, __ATOMIC_RELAXED);
    return tab;
}

struct efs_export *efs_export_table_for_ino(struct efs_export *ex, efs_ino_t ino)
{
    if (!ex)
        return NULL;
    if (ex->root.shard_bits == 0)
        return ex;
    return efs_export_table(ex, efs_export_shard_of(ino, ex->root.shard_bits));
}

struct efs_export *efs_export_shard_tab(struct efs_export *ex, uint32_t shard)
{
    if (!ex)
        return NULL;
    if (shard == 0 || ex->root.shard_bits == 0 ||
        (ex->root.shard_count ? ex->root.shard_count : 1) <= 1)
        return ex;
    if (!ex->shard_tabs || shard >= ex->shard_tab_cap)
        return NULL;
    return ex->shard_tabs[shard];
}

/* Extra tabs carry the same shard_bits/shard_count, so the main table has to
 * be told apart by shard_id -- shard 0 is always `ex` itself, a tab is never
 * created for it. This used to test for a resident ROOT row, which is also
 * true of the main table in steady state but NOT on a joiner that has not
 * caught up yet: lookup then fell through to the flat single-table path and
 * answered ENOENT for every name living on an extra shard. */
static int export_is_sharded_root(struct efs_export *ex)
{
    return ex && ex->root.shard_bits && ex->root.shard_count > 1 &&
           ex->shard_id == 0;
}

static int lookup_on_tab(struct efs_export *tab, efs_ino_t parent,
                         const char *name, struct efs_inode *out)
{
    uint64_t pos = 0;
    if (!tab || !name || name_idx_get(tab, parent, name, &pos) != 0)
        return EFS_ERR_NOT_FOUND;
    if (out)
        efs_export_inode_to_rpc(tab, pos, out);
    return EFS_OK;
}

static void lookup_stitch_child(struct efs_export *ex, struct efs_inode *out)
{
    if (!ex || !out || !export_is_sharded_root(ex))
        return;
    uint32_t bits = ex->root.shard_bits;
    uint32_t csh = efs_export_shard_of(out->ino, bits);
    struct efs_export *ctab = (csh == 0) ? ex : efs_export_shard_tab(ex, csh);
    struct efs_inode full;
    if (ctab && efs_export_get_inode(ctab, out->ino, &full) == 0) {
        char nbuf[EFS_MAX_NAME];
        memcpy(nbuf, out->name, EFS_MAX_NAME);
        efs_ino_t p = out->parent;
        *out = full;
        memcpy(out->name, nbuf, EFS_MAX_NAME);
        out->parent = p;
    }
}

int efs_export_lookup(struct efs_export *ex, efs_ino_t parent,
                      const char *name, struct efs_inode *out)
{
    if (!ex || !name)
        return EFS_ERR_INVAL;

    if (export_is_sharded_root(ex)) {
        uint32_t bits = ex->root.shard_bits;
        uint32_t psh = efs_export_shard_of(parent, bits);
        /* Dentry shards only. Walking every extra tab found the child-row
         * name (create_with_ino writes parent+name on ctab), so rename and
         * unlink left exists(old) true after the parent dentry was gone. */
        struct efs_export *ptab = (psh == 0) ? ex
            : efs_export_shard_tab(ex, psh);
        /* Spread dentries AND hashed ROOT dirs (posix testdir, ecopy dest)
         * live on hash(parent, name), not only the parent shard. Cut 4
         * LOOKUP no longer has a local replica to hide a parent-stub miss. */
        if (parent == EFS_ROOT_INO || efs_export_dir_is_spread(ex, parent)) {
            uint32_t dsh = efs_export_dentry_shard_of(parent, name, bits);
            if (dsh != psh) {
                struct efs_export *htab = (dsh == 0) ? ex
                    : efs_export_shard_tab(ex, dsh);
                if (htab && lookup_on_tab(htab, parent, name, out) == EFS_OK) {
                    if (out)
                        lookup_stitch_child(ex, out);
                    return EFS_OK;
                }
            }
        }
        if (!ptab)
            return EFS_ERR_NOT_FOUND;
        int rc = lookup_on_tab(ptab, parent, name, out);
        if (rc == EFS_OK && out)
            lookup_stitch_child(ex, out);
        return rc;
    }

    uint64_t pos = 0;
    if (name_idx_get(ex, parent, name, &pos) == 0) {
        if (out)
            efs_export_inode_to_rpc(ex, pos, out);
        if (out)
            lookup_stitch_child(ex, out);
        return EFS_OK;
    }
    return EFS_ERR_NOT_FOUND;
}

int efs_export_get_inode(struct efs_export *ex, efs_ino_t ino,
                         struct efs_inode *out)
{
    if (!ex)
        return EFS_ERR_INVAL;
    if (export_is_sharded_root(ex)) {
        uint32_t sh = efs_export_shard_of(ino, ex->root.shard_bits);
        if (sh != 0) {
            struct efs_export *tab = efs_export_shard_tab(ex, sh);
            if (!tab)
                return EFS_ERR_NOT_FOUND;
            return efs_export_get_inode(tab, ino, out);
        }
    }
    struct efs_inode_mem *p = inode_ptr(ex, ino);
    if (!p)
        return EFS_ERR_NOT_FOUND;
    if (out)
        inode_to_rpc_p(ex, p, out);
    return EFS_OK;
}

efs_ino_t efs_export_create_with_ino(struct efs_export *ex, efs_ino_t ino_num,
                                     efs_ino_t parent, uint32_t mode,
                                     uid_t uid, gid_t gid, const char *name)
{
    if (!ex || !name || !*name || ino_num == 0)
        return 0;
    if (strcmp(name, EFS_STATS_NAME) == 0)
        return 0;

    efs_export_ensure_rollups(ex);

    /* This insert is table-local. efs_export_lookup on a sharded root
     * walks every loaded extra tab, so the child-row we just wrote on
     * ctab makes the parent-dentry create_with_ino look like EEXIST —
     * readdir of the parent then misses every file whose inode shard
     * is instantiated on this node (1/8 of creates on bits=3). */
    if (lookup_on_tab(ex, parent, name, NULL) == EFS_OK) {
        /* Skipping the dual-apply here is not a correctness event: the name is
         * already on this table, and since lookup_walk resolves every name
         * against the owning server the local row is not authoritative anyway.
         * ino_dup below stays loud — a reissued live ino corrupts a file. */
        return 0;
    }

    {
    struct efs_inode_mem *dupp = inode_ptr(ex, ino_num);
    if (dupp) {
        fprintf(stderr, "cwi-fail: ino_dup parent=%llu name=%s ino=%llu "
                "shard=%u cnt=%llu held_by parent=%llu name=%s mode=%o\n",
                (unsigned long long)parent, name,
                (unsigned long long)ino_num, ex->shard_id,
                (unsigned long long)ex->inode_count,
                (unsigned long long)dupp->parent,
                inamep(ex, dupp), dupp->mode);
        return 0; /* ino already in use on this table */
    }
    }

    if (ex->inode_count >= ex->inode_capacity) {
        uint64_t new_cap = ex->inode_capacity * 2;
        if (inode_ensure_cap(ex, new_cap) != EFS_OK) {
            fprintf(stderr, "cwi-fail: realloc parent=%llu name=%s ino=%llu\n",
                    (unsigned long long)parent, name,
                    (unsigned long long)ino_num);
            return 0;
        }
    }
    if (export_ensure_inode_idx(ex) != 0) {
        fprintf(stderr, "cwi-fail: idx parent=%llu name=%s ino=%llu\n",
                (unsigned long long)parent, name,
                (unsigned long long)ino_num);
        return 0;
    }

    uint64_t pos = ex->inode_count++;
    struct efs_inode_mem *ino = inode_at(ex, pos);
    if (!ino) {
        ex->inode_count--;
        return 0;
    }
    memset(ino, 0, sizeof(*ino));
    ino->ino = ino_num;
    ino->parent = parent;
    ino->mode = mode;
    ino->uid = uid;
    ino->gid = gid;
    ino->nlink = 1;
    time_now(&ino->mtime);
    ino->ctime = ino->mtime;
    ino->atime = ino->mtime;
    if (inode_set_name(ex, ino, name) != 0) {
        ex->inode_count--;
        return 0;
    }
    dentry_bytes_add(ex, name);

    idx_put(ex->ino_keys, ex->ino_vals, ex->ino_mask, ino_num, pos);
    name_idx_put(ex, parent, name, pos);
    child_idx_add(ex, parent, pos);

    if (efs_mode_is_dir(mode)) {
        ino->nlink = 2;
        struct efs_inode_mem *p = inode_ptr(ex, parent);
        if (p)
            p->nlink++;
    }

    rollup_add_under(ex, parent, ino);
    parent_touch(ex, parent);
    /* Unsharded create (bits=0) used to leave shard_dirty clear: the flush
     * thread saw rpc_dirty, called flush, then skipped shard 0 — RAM-only
     * mutations and the 70k-ops/s "bench" that never hit disk. */
    ex->shard_dirty = 1;
    return ino->ino;
}

/* Touch already-materialized extra tables only. Instantiating every shard
 * here (the old unlink/nlink loops) made the parent owner hold the world.
 * skip_shard is the canonical child table (owner applies nlink/chunks). */
static void for_each_loaded_tab(struct efs_export *ex, uint32_t skip_shard,
                                void (*fn)(struct efs_export *tab, efs_ino_t ino,
                                           uint32_t nlink),
                                efs_ino_t ino, uint32_t nlink)
{
    if (ex->shard_id != skip_shard)
        fn(ex, ino, nlink);
    if (!ex->shard_tabs)
        return;
    for (uint32_t i = 1; i < ex->shard_tab_cap; i++) {
        if (ex->shard_tabs[i] && i != skip_shard)
            fn(ex->shard_tabs[i], ino, nlink);
    }
}

static void tab_set_nlink(struct efs_export *tab, efs_ino_t ino, uint32_t nlink)
{
    /* Same authoritative-index early-out as efs_export_unlink: this is fanned
     * across every loaded shard table, so the scan must not run on tables that
     * cannot hold the ino. The scan still runs when the ino IS present, since
     * hardlinks put several rows on one table and the index holds only one. */
    if (tab->ino_keys && !inode_ptr(tab, ino))
        return;
    int hit = 0;
    for (uint64_t i = 0; i < tab->inode_count; i++) {
        if (inode_at(tab, i)->ino == ino) {
            inode_at(tab, i)->nlink = nlink;
            hit = 1;
        }
    }
    if (hit)
        tab->shard_dirty = 1;
}

int efs_export_unlink_name_ex(struct efs_export *ex, efs_ino_t parent,
                              const char *name, int keep_last)
{
    if (!ex || !name)
        return EFS_ERR_INVAL;
    if (export_is_sharded_root(ex)) {
        uint32_t bits = ex->root.shard_bits;
        uint32_t psh = efs_export_shard_of(parent, bits);
        struct efs_export *ptab = efs_export_table(ex, psh);
        if (!ptab)
            return EFS_ERR_NOT_FOUND;

        /* Spread: hash-shard dentry first, parent-shard leftover second.
         * Crash between dest-create and source-delete may leave both;
         * hash-shard row wins, parent-shard is the duplicate we drop. */
        struct efs_export *dtab = ptab;
        uint64_t pos = 0;
        if (efs_export_dir_is_spread(ex, parent)) {
            uint32_t dsh = efs_export_dentry_shard_of(parent, name, bits);
            struct efs_export *htab = (dsh == 0) ? ex
                : efs_export_shard_tab(ex, dsh);
            if (htab && name_idx_get(htab, parent, name, &pos) == 0)
                dtab = htab;
        }
        if (dtab == ptab && name_idx_get(ptab, parent, name, &pos) != 0)
            return EFS_ERR_NOT_FOUND;
        if (dtab != ptab && name_idx_get(dtab, parent, name, &pos) != 0)
            return EFS_ERR_NOT_FOUND;
        struct efs_inode_mem removed = (*inode_at(dtab, pos));
        efs_ino_t ino = removed.ino;

        if (efs_mode_is_dir(removed.mode)) {
            if (efs_export_unlink(dtab, ino) == EFS_OK)
                dtab->shard_dirty = 1;
            if (dtab != ptab)
                rollup_sub_under(ptab, parent, &removed);
            parent_touch(ptab, parent);
            return EFS_OK;
        }

        uint32_t nlink = removed.nlink;
        if (nlink == 0)
            nlink = 1;
        nlink--;
        rollup_sub_under(ptab, parent, &removed);
        child_idx_del(dtab, parent, pos);
        parent_touch(ptab, parent);
        /* Last-link + keep_last: if this dentry slot is the canonical inode
         * row (file create co-locates them), converting it to a nameless
         * ghost keeps size/chunks for an already-open fd. Removing it made
         * getattr miss locally, RPC-adopt the owner's still-0 size, and
         * fuse_file_read_iter (attr_timeout=0) zero kernel i_size → EOF.
         * A hardlink dentry on a different table is dropped here; the
         * caller's nlink_dec_ex ghosts the child table. */
        if (nlink == 0 && keep_last) {
            struct efs_export *ctab = efs_export_table_for_ino(ex, ino);
            if (!ctab || ctab == dtab) {
                char nm[EFS_MAX_NAME];
                struct efs_inode_mem *row = inode_at(dtab, pos);
                memset(nm, 0, sizeof(nm));
                if (row)
                    strncpy(nm, inamep(dtab, row), EFS_MAX_NAME - 1);
                name_idx_del(dtab, parent, nm);
                dentry_bytes_sub(dtab, nm);
                if (row) {
                    row->nlink = 0;
                    inode_set_name(dtab, row, "");
                    row->parent = 0;
                }
                dtab->shard_dirty = 1;
                if (ptab != dtab)
                    ptab->shard_dirty = 1;
                return EFS_OK;
            }
        }
        remove_inode_slot(dtab, pos, nlink > 0);
        dtab->shard_dirty = 1;
        if (ptab != dtab)
            ptab->shard_dirty = 1;

        /* Parent dentry only when the canonical row lives elsewhere.
         * Child nlink / last-link drop is the caller's nlink_dec_ex. */
        return EFS_OK;
    }
    if (strcmp(name, EFS_STATS_NAME) == 0)
        return EFS_ERR_INVAL;
    efs_export_ensure_rollups(ex);

    uint64_t pos = 0;
    if (name_idx_get(ex, parent, name, &pos) != 0)
        return EFS_ERR_NOT_FOUND;

    struct efs_inode_mem removed = (*inode_at(ex, pos));
    efs_ino_t ino = removed.ino;
    uint32_t nlink = removed.nlink;
    if (nlink == 0)
        nlink = 1;
    nlink--;

    /* POSIX: a subdirectory's ".." holds a link on the parent. */
    if (efs_mode_is_dir(removed.mode)) {
        struct efs_inode_mem *p = inode_ptr(ex, parent);
        if (p && p->nlink > 2)
            p->nlink--;
    }

    rollup_sub_under(ex, parent, &removed);
    child_idx_del(ex, parent, pos);
    parent_touch(ex, parent);
    if (nlink == 0 && keep_last) {
        /* Drop the directory name but keep the inode + chunks so an
         * already-open fd on another client can still read. */
        name_idx_del(ex, parent, inamep(ex, &removed));
        dentry_bytes_sub(ex, inamep(ex, &removed));
        inode_at(ex, pos)->nlink = 0;
        inode_set_name(ex, inode_at(ex, pos), "");
        inode_at(ex, pos)->parent = 0;
        ex->shard_dirty = 1;
        return EFS_OK;
    }

    remove_inode_slot(ex, pos, nlink > 0);
    ex->shard_dirty = 1;

    if (nlink == 0) {
        remove_chunks_for_ino(ex, ino);
        return EFS_OK;
    }

    /* Remaining hard links keep the decremented nlink. */
    for (uint64_t i = 0; i < ex->inode_count; i++) {
        if (inode_at(ex, i)->ino == ino)
            inode_at(ex, i)->nlink = nlink;
    }
    return EFS_OK;
}

int efs_export_unlink(struct efs_export *ex, efs_ino_t ino)
{
    if (!ex)
        return EFS_ERR_INVAL;

    /* The ino index is authoritative (see inode_ptr): a miss means this table
     * holds no row for the ino, so the scan below cannot find one. Skip it.
     * unlink/rename fan this call across EVERY loaded shard table
     * (for_each_loaded_tab), so without the early-out a single unlink costs
     * O(total inodes in the export) — the same full-scan-under-a-fan shape
     * that made stamp_ctime_loaded 9% of efsd on a grown table. */
    if (ex->ino_keys && !inode_ptr(ex, ino))
        return EFS_ERR_NOT_FOUND;

    /* Remove every directory name for this inode, then chunks. */
    int found = 0;
    uint64_t i = 0;
    while (i < ex->inode_count) {
        if (inode_at(ex, i)->ino == ino) {
            found = 1;
            struct efs_inode_mem removed = (*inode_at(ex, i));
            if (efs_mode_is_dir(removed.mode)) {
                struct efs_inode_mem *p = inode_ptr(ex, removed.parent);
                if (p && p->nlink > 2)
                    p->nlink--;
            }
            rollup_sub_under(ex, removed.parent, &removed);
            child_idx_del(ex, removed.parent, i);
            remove_inode_slot(ex, i, 0);
            /* slot i now holds a different row (or count shrank) */
        } else {
            i++;
        }
    }
    if (!found)
        return EFS_ERR_NOT_FOUND;
    remove_chunks_for_ino(ex, ino);
    return EFS_OK;
}

static void shard_set_nlink(struct efs_export *ex, efs_ino_t src_ino,
                            uint32_t nlink)
{
    for_each_loaded_tab(ex, UINT32_MAX, tab_set_nlink, src_ino, nlink);
}

int efs_export_nlink_inc(struct efs_export *ex, efs_ino_t src_ino,
                         struct efs_inode *out)
{
    if (!ex || !src_ino)
        return EFS_ERR_INVAL;
    struct efs_export *ctab = efs_export_table_for_ino(ex, src_ino);
    struct efs_inode_mem *csrc = ctab ? inode_ptr(ctab, src_ino) : NULL;
    if (!csrc)
        return EFS_ERR_NOT_FOUND;
    if (efs_mode_is_dir(csrc->mode))
        return EFS_ERR_INVAL;
    inode_bump_ctime(csrc);
    uint64_t ct = csrc->ctime;
    uint32_t nlink = csrc->nlink + 1;
    shard_set_nlink(ex, src_ino, nlink);
    stamp_ctime_loaded(ex, src_ino, ct);
    ctab->shard_dirty = 1;
    if (out) {
        csrc = inode_ptr(ctab, src_ino);
        if (csrc)
            inode_to_rpc_p(ctab, csrc, out);
    }
    return EFS_OK;
}

int efs_export_nlink_dec_ex(struct efs_export *ex, efs_ino_t src_ino,
                            struct efs_inode *out, int keep_last)
{
    if (!ex || !src_ino)
        return EFS_ERR_INVAL;
    struct efs_export *ctab = efs_export_table_for_ino(ex, src_ino);
    struct efs_inode_mem *csrc = ctab ? inode_ptr(ctab, src_ino) : NULL;
    if (!csrc)
        return EFS_ERR_NOT_FOUND;
    uint32_t nlink = csrc->nlink;
    if (nlink == 0)
        nlink = 1;
    nlink--;
    if (nlink == 0) {
        if (keep_last) {
            shard_set_nlink(ex, src_ino, 0);
            csrc = inode_ptr(ctab, src_ino);
            if (csrc) {
                inode_set_name(ctab, csrc, "");
                csrc->parent = 0;
            }
            ctab->shard_dirty = 1;
            if (out && csrc)
                inode_to_rpc_p(ctab, csrc, out);
            return EFS_OK;
        }
        /* Canonical child table only — lock_all is gone from UNLINK. */
        if (efs_export_unlink(ctab, src_ino) == EFS_OK)
            ctab->shard_dirty = 1;
        if (out)
            memset(out, 0, sizeof(*out));
        return EFS_OK;
    }
    shard_set_nlink(ex, src_ino, nlink);
    ctab->shard_dirty = 1;
    if (out) {
        csrc = inode_ptr(ctab, src_ino);
        if (csrc)
            inode_to_rpc_p(ctab, csrc, out);
    }
    return EFS_OK;
}

int efs_export_link_dentry(struct efs_export *ex, const struct efs_inode *src,
                           efs_ino_t new_parent, const char *new_name)
{
    if (!ex || !src || !src->ino || !new_name || !*new_name)
        return EFS_ERR_INVAL;
    if (strcmp(new_name, EFS_STATS_NAME) == 0)
        return EFS_ERR_INVAL;
    if (efs_mode_is_dir(src->mode))
        return EFS_ERR_INVAL;
    if (efs_export_lookup(ex, new_parent, new_name, NULL) == EFS_OK)
        return EFS_ERR_EXIST;

    uint32_t bits = ex->root.shard_bits;
    struct efs_export *ptab =
        export_is_sharded_root(ex)
            ? efs_export_table(ex, efs_export_shard_of(new_parent, bits))
            : ex;
    struct efs_inode_mem *pdir = ptab ? inode_ptr(ptab, new_parent) : NULL;
    if (!pdir || !efs_mode_is_dir(pdir->mode))
        return EFS_ERR_INVAL;

    efs_export_ensure_rollups(ptab);
    if (ptab->inode_count >= ptab->inode_capacity) {
        uint64_t new_cap = ptab->inode_capacity * 2;
        if (inode_ensure_cap(ptab, new_cap) != EFS_OK)
            return EFS_ERR_NOMEM;
    }
    if (export_ensure_inode_idx(ptab) != 0)
        return EFS_ERR_NOMEM;

    shard_set_nlink(ex, src->ino, src->nlink);

    uint64_t pos = ptab->inode_count++;
    struct efs_inode_mem *dst = inode_at(ptab, pos);
    if (inode_from_rpc(ptab, dst, src) != 0) {
        ptab->inode_count--;
        return EFS_ERR_NOMEM;
    }
    dst->parent = new_parent;
    dst->nlink = src->nlink;
    inode_clear_rollups(dst);
    if (inode_set_name(ptab, dst, new_name) != 0) {
        ptab->inode_count--;
        return EFS_ERR_NOMEM;
    }
    dentry_bytes_add(ptab, new_name);
    time_now(&dst->ctime);
    name_idx_put(ptab, new_parent, new_name, pos);
    child_idx_add(ptab, new_parent, pos);
    rollup_add_under(ptab, new_parent, dst);
    parent_touch(ptab, new_parent);
    ptab->shard_dirty = 1;
    return EFS_OK;
}

int efs_export_link(struct efs_export *ex, efs_ino_t src_ino,
                    efs_ino_t new_parent, const char *new_name)
{
    if (!ex || !new_name || !*new_name)
        return EFS_ERR_INVAL;
    if (strcmp(new_name, EFS_STATS_NAME) == 0)
        return EFS_ERR_INVAL;

    /* Sharded root: the canonical src row lives on the CHILD shard's table
     * and the new dentry belongs on the NEW PARENT's shard table. The
     * unsharded path below would look src up only in the main (shard-0)
     * table — NOT_FOUND for any other shard, so the link never landed
     * server-side (posix hardlink_terminal_ln on bits>0). When the child
     * row is on a peer, the caller (LINK handler) nlink_inc's remotely
     * and uses efs_export_link_dentry. */
    if (export_is_sharded_root(ex)) {
        if (efs_export_lookup(ex, new_parent, new_name, NULL) == EFS_OK)
            return EFS_ERR_EXIST;
        struct efs_inode bumped;
        memset(&bumped, 0, sizeof(bumped));
        int rc = efs_export_nlink_inc(ex, src_ino, &bumped);
        if (rc != EFS_OK)
            return rc;
        return efs_export_link_dentry(ex, &bumped, new_parent, new_name);
    }

    efs_export_ensure_rollups(ex);

    struct efs_inode_mem *src = inode_ptr(ex, src_ino);
    if (!src)
        return EFS_ERR_NOT_FOUND;
    if (efs_mode_is_dir(src->mode))
        return EFS_ERR_INVAL;
    if (efs_export_lookup(ex, new_parent, new_name, NULL) == EFS_OK)
        return EFS_ERR_EXIST;
    struct efs_inode_mem *pdir = inode_ptr(ex, new_parent);
    if (!pdir || !efs_mode_is_dir(pdir->mode))
        return EFS_ERR_INVAL;

    if (ex->inode_count >= ex->inode_capacity) {
        uint64_t new_cap = ex->inode_capacity * 2;
        if (inode_ensure_cap(ex, new_cap) != EFS_OK)
            return EFS_ERR_NOMEM;
    }
    if (export_ensure_inode_idx(ex) != 0)
        return EFS_ERR_NOMEM;

    /* Re-fetch after possible realloc. */
    src = inode_ptr(ex, src_ino);
    if (!src)
        return EFS_ERR_NOT_FOUND;

    inode_bump_ctime(src);
    uint64_t ct = src->ctime;
    uint32_t nlink = src->nlink + 1;
    sync_hardlink_attrs(ex, src_ino, src);
    for (uint64_t i = 0; i < ex->inode_count; i++) {
        if (inode_at(ex, i)->ino == src_ino)
            inode_at(ex, i)->nlink = nlink;
    }
    stamp_ctime_loaded(ex, src_ino, ct);
    src = inode_ptr(ex, src_ino);

    uint64_t pos = ex->inode_count++;
    struct efs_inode_mem *dst = inode_at(ex, pos);
    *dst = *src;
    /* The copy carried src's arena offset; the new row owns no name bytes
     * yet, so clear them before interning new_name into dst's own slab. */
    dst->slab_idx = (uint16_t)(pos / EFS_INO_SLAB_ROWS);
    dst->name_off = 0;
    dst->name_len = 0;
    dst->parent = new_parent;
    dst->nlink = nlink;
    inode_clear_rollups(dst);
    if (inode_set_name(ex, dst, new_name) != 0) {
        ex->inode_count--;
        return EFS_ERR_NOMEM;
    }
    dentry_bytes_add(ex, new_name);
    time_now(&dst->ctime);

    /* Keep ino_idx pointing at an existing row; name index gets the new name. */
    name_idx_put(ex, new_parent, new_name, pos);
    child_idx_add(ex, new_parent, pos);
    rollup_add_under(ex, new_parent, dst);
    parent_touch(ex, new_parent);
    ex->shard_dirty = 1;
    return EFS_OK;
}

/* Attr accessors want the canonical row, which on a sharded export lives on
 * the child shard's table (same routing as efs_export_get_inode). The
 * parent-shard dentry copy carries name/parent only. */
static struct efs_export *shard_route(struct efs_export *ex, efs_ino_t ino)
{
    if (export_is_sharded_root(ex)) {
        struct efs_export *tab = efs_export_table_for_ino(ex, ino);
        if (tab)
            return tab;
    }
    return ex;
}

static int set_size_common(struct efs_export *ex, efs_ino_t ino, uint64_t size,
                           int do_rollups)
{
    if (!ex)
        return EFS_ERR_INVAL;
    ex = shard_route(ex, ino);
    if (do_rollups)
        efs_export_ensure_rollups(ex);
    struct efs_inode_mem *p = inode_ptr(ex, ino);
    if (!p)
        return EFS_ERR_NOT_FOUND;
    uint64_t old_size = p->size;
    p->size = size;
    /* Size changes update mtime with full nsec precision (not sec-only). */
    {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        p->mtime = (uint64_t)ts.tv_sec;
        p->mtime_nsec = (uint32_t)ts.tv_nsec;
    }
    sync_hardlink_attrs(ex, ino, p);
    if (!do_rollups) {
        int64_t delta = 0;
        if (!efs_mode_is_dir(p->mode) && size != old_size)
            delta = (int64_t)size - (int64_t)old_size;
        if (pending_rollup_note(ex, ino, delta, 1) != 0)
            return EFS_ERR_NOMEM;
        ex->rollups_stale = 1;
        return EFS_OK;
    }
    if (!efs_mode_is_dir(p->mode) && size != old_size) {
        int64_t delta = (int64_t)size - (int64_t)old_size;
        apply_size_delta_for_ino(ex, ino, delta);
    }
    rollup_expand_parents_of(ex, ino); /* set_size stamps mtime=now: expand */
    return EFS_OK;
}

int efs_export_set_size(struct efs_export *ex, efs_ino_t ino, uint64_t size)
{
    return set_size_common(ex, ino, size, 1);
}

int efs_export_set_size_norollup(struct efs_export *ex, efs_ino_t ino,
                                 uint64_t size)
{
    return set_size_common(ex, ino, size, 0);
}

int efs_export_set_mode(struct efs_export *ex, efs_ino_t ino, uint32_t mode)
{
    if (!ex)
        return EFS_ERR_INVAL;
    ex = shard_route(ex, ino);
    efs_export_ensure_rollups(ex);
    struct efs_inode_mem *p = inode_ptr(ex, ino);
    if (!p)
        return EFS_ERR_NOT_FOUND;
    p->mode = (p->mode & S_IFMT) | (mode & ~S_IFMT);
    /* POSIX: chmod updates ctime, not mtime (rsync -a relies on this).
     * ctime is stored at second resolution; if chmod lands in the same
     * second, still advance so st_ctime_ns is observably newer. */
    {
        uint64_t old = p->ctime;
        time_now(&p->ctime);
        if (p->ctime <= old)
            p->ctime = old + 1;
    }
    sync_hardlink_attrs(ex, ino, p);
    rollup_expand_parents_of(ex, ino); /* ctime=now: expand */
    return EFS_OK;
}

int efs_export_set_owner(struct efs_export *ex, efs_ino_t ino, uid_t uid, gid_t gid)
{
    if (!ex)
        return EFS_ERR_INVAL;
    ex = shard_route(ex, ino);
    efs_export_ensure_rollups(ex);
    struct efs_inode_mem *p = inode_ptr(ex, ino);
    if (!p)
        return EFS_ERR_NOT_FOUND;
    if (uid != (uid_t)-1)
        p->uid = uid;
    if (gid != (gid_t)-1)
        p->gid = gid;
    /* POSIX: chown updates ctime, not mtime. */
    time_now(&p->ctime);
    sync_hardlink_attrs(ex, ino, p);
    rollup_expand_parents_of(ex, ino); /* ctime=now: expand */
    return EFS_OK;
}

static int set_mtime_ns_common(struct efs_export *ex, efs_ino_t ino,
                               uint64_t mtime, uint32_t mtime_nsec,
                               int do_rollups)
{
    if (!ex)
        return EFS_ERR_INVAL;
    ex = shard_route(ex, ino);
    if (do_rollups)
        efs_export_ensure_rollups(ex);
    struct efs_inode_mem *p = inode_ptr(ex, ino);
    if (!p)
        return EFS_ERR_NOT_FOUND;
    if (mtime_nsec >= 1000000000u)
        mtime_nsec = 0;
    p->mtime = mtime;
    p->mtime_nsec = mtime_nsec;
    sync_hardlink_attrs(ex, ino, p);
    if (!do_rollups) {
        if (pending_rollup_note(ex, ino, 0, 1) != 0)
            return EFS_ERR_NOMEM;
        ex->rollups_stale = 1;
        return EFS_OK;
    }
    /* entry_tmax is dominated by ctime (=now, never back-dated), so even an
     * explicit utimens only widens the parent range — expand incrementally. */
    rollup_expand_parents_of(ex, ino);
    return EFS_OK;
}

int efs_export_set_mtime_ns(struct efs_export *ex, efs_ino_t ino,
                            uint64_t mtime, uint32_t mtime_nsec)
{
    return set_mtime_ns_common(ex, ino, mtime, mtime_nsec, 1);
}

int efs_export_set_mtime_ns_norollup(struct efs_export *ex, efs_ino_t ino,
                                     uint64_t mtime, uint32_t mtime_nsec)
{
    return set_mtime_ns_common(ex, ino, mtime, mtime_nsec, 0);
}

int efs_export_set_mtime(struct efs_export *ex, efs_ino_t ino, uint64_t mtime)
{
    return efs_export_set_mtime_ns(ex, ino, mtime, 0);
}

int efs_export_set_atime(struct efs_export *ex, efs_ino_t ino, uint64_t atime)
{
    if (!ex)
        return EFS_ERR_INVAL;
    ex = shard_route(ex, ino);
    efs_export_ensure_rollups(ex);
    struct efs_inode_mem *p = inode_ptr(ex, ino);
    if (!p)
        return EFS_ERR_NOT_FOUND;
    p->atime = atime;
    sync_hardlink_attrs(ex, ino, p);
    rollup_expand_parents_of(ex, ino);
    return EFS_OK;
}

int efs_export_rename_at(struct efs_export *ex, efs_ino_t old_parent,
                         const char *old_name, efs_ino_t new_parent,
                         const char *new_name)
{
    if (!ex || !old_name || !new_name)
        return EFS_ERR_INVAL;
    struct efs_export *tab = ex;
    if (export_is_sharded_root(ex) && ex->root.shard_bits) {
        uint32_t sh = efs_export_shard_of(old_parent, ex->root.shard_bits);
        if (sh != 0) {
            tab = efs_export_table(ex, sh);
            if (!tab)
                return EFS_ERR_NOT_FOUND;
        }
    }
    uint64_t pos = 0;
    if (name_idx_get(tab, old_parent, old_name, &pos) != 0)
        return EFS_ERR_NOT_FOUND;
    efs_ino_t ino = inode_at(tab, pos)->ino;
    if (tab->ino_keys)
        idx_put(tab->ino_keys, tab->ino_vals, tab->ino_mask, ino, pos);
    return efs_export_rename(tab == ex ? ex : tab, ino, new_parent, new_name);
}

int efs_export_rename(struct efs_export *ex, efs_ino_t ino,
                      efs_ino_t new_parent, const char *new_name)
{
    if (!ex || !new_name)
        return EFS_ERR_INVAL;
    if (strcmp(new_name, EFS_STATS_NAME) == 0)
        return EFS_ERR_INVAL;
    /* Files live on the parent directory's extra-shard tab. inode_ptr on
     * the main table misses them (efs-bench RENAME 5000/5000 NOT_FOUND;
     * setattr already shard_route'd). Same-dir nested trees stay on one
     * tab; rename_at already passed that tab and is not a sharded root. */
    if (export_is_sharded_root(ex)) {
        struct efs_export *tab = efs_export_table_for_ino(ex, ino);
        if (tab)
            ex = tab;
    }
    efs_export_ensure_rollups(ex);

    struct efs_inode_mem *src = inode_ptr(ex, ino);
    if (!src)
        return EFS_ERR_NOT_FOUND;

    struct efs_inode_mem *dst_parent = inode_ptr(ex, new_parent);
    if (!dst_parent || !efs_mode_is_dir(dst_parent->mode))
        return EFS_ERR_INVAL;

    /* Prevent moving a directory into itself or its descendants. */
    if (efs_mode_is_dir(src->mode)) {
        efs_ino_t p = new_parent;
        while (p != EFS_ROOT_INO && p != src->ino) {
            struct efs_inode_mem *pi = inode_ptr(ex, p);
            if (!pi || pi->parent == p)
                break;
            p = pi->parent;
        }
        if (p == src->ino)
            return EFS_ERR_INVAL;
    }

    struct efs_inode_mem dst_copy;
    int have_dst = 0;
    {
        uint64_t dpos = 0;
        if (name_idx_get(ex, new_parent, new_name, &dpos) == 0 &&
            inode_at(ex, dpos)->ino != ino) {
            dst_copy = (*inode_at(ex, dpos));
            have_dst = 1;
        }
    }

    if (have_dst) {
        int src_dir = efs_mode_is_dir(src->mode);
        int dst_dir = efs_mode_is_dir(dst_copy.mode);

        if (src_dir != dst_dir)
            return EFS_ERR_INVAL;

        if (src_dir) {
            struct efs_child_vec *cv = child_vec_get(ex, dst_copy.ino, 0);
            if (cv && cv->count > 0)
                return EFS_ERR_NOT_EMPTY;
        }
        /* efs_export_unlink swap-removes the destination entry, which can
         * move or overwrite the entry `src` points to. Re-find it after. */
        efs_export_unlink(ex, dst_copy.ino);
        src = inode_ptr(ex, ino);
        if (!src)
            return EFS_ERR_NOT_FOUND;
    }

    uint64_t slot = inode_slot_of(ex, src);
    efs_ino_t old_parent = src->parent;
    if (old_parent != new_parent || strcmp(inamep(ex, src), new_name) != 0) {
        struct efs_inode_mem snap = *src;
        char old_name[EFS_MAX_NAME];
        memset(old_name, 0, sizeof(old_name));
        strncpy(old_name, inamep(ex, src), EFS_MAX_NAME - 1);
        if (old_parent != new_parent)
            rollup_sub_under(ex, old_parent, &snap);
        /* Same-dir rename: slot stays in the parent child-vec. */
        if (old_parent != new_parent)
            child_idx_del(ex, old_parent, slot);
        name_idx_del(ex, src->parent, old_name);
        dentry_bytes_sub(ex, old_name);
        if (inode_set_name(ex, src, new_name) != 0)
            return EFS_ERR_NOMEM;
        dentry_bytes_add(ex, new_name);
        src->parent = new_parent;
        /* Preserve mtime across rename (rsync partial → final); bump ctime
         * on every hard-link row so lstat of the other name sees it. */
        inode_bump_ctime(src);
        stamp_ctime_loaded(ex, ino, src->ctime);
        name_idx_put(ex, src->parent, new_name, slot);
        if (old_parent != new_parent)
            child_idx_add(ex, new_parent, slot);
        if (old_parent != new_parent) {
            if (efs_mode_is_dir(src->mode)) {
                struct efs_inode_mem *op = inode_ptr(ex, old_parent);
                struct efs_inode_mem *np = inode_ptr(ex, new_parent);
                if (op && op->nlink > 2)
                    op->nlink--;
                if (np)
                    np->nlink++;
            }
            rollup_add_under(ex, new_parent, src);
            parent_touch(ex, old_parent);
            parent_touch(ex, new_parent);
        } else {
            /* Same-dir rename only bumps ctime (time-forward). Expand
             * in O(depth) — recompute_times_up rescans every child and
             * made rsync temp→final O(n²) in a wide directory. */
            expand_parent_chain(ex, src->ino, new_parent,
                                entry_tmin(src), entry_tmax(src));
            parent_touch(ex, new_parent);
        }
    }
    /* Same-count rename only rewrites the dentry tail and the compact slot.
     * mark_full would disable incremental serialize for the whole ecopy
     * temp→final storm (every file is a rename). Overwrite already mark_full
     * via unlink → remove_inode_slot. */
    ex->shard_dirty = 1;
    return EFS_OK;
}

int efs_export_set_chunk(struct efs_export *ex, efs_ino_t ino, uint32_t chunk_index,
                         const efs_node_id_t fragment_nodes[EFS_NUM_FRAGMENTS],
                         const uint8_t checksums[EFS_NUM_FRAGMENTS][EFS_HASH_SIZE])
{
    if (!ex)
        return EFS_ERR_INVAL;
    if (export_is_sharded_root(ex)) {
        struct efs_export *tab = efs_export_table_for_chunk(ex, ino,
                                                           chunk_index);
        if (tab && tab != ex)
            return efs_export_set_chunk(tab, ino, chunk_index, fragment_nodes,
                                        checksums);
    }

    uint64_t pos = 0;
    if (chunk_idx_get(ex, ino, chunk_index, &pos) == 0) {
        memcpy(ex->chunks[pos].fragment_nodes, fragment_nodes,
               sizeof(efs_node_id_t) * EFS_NUM_FRAGMENTS);
        memcpy(ex->chunks[pos].checksums, checksums,
               EFS_HASH_SIZE * EFS_NUM_FRAGMENTS);
        ex->chunk_epoch++;
        return EFS_OK;
    }

    if (ex->chunk_count >= ex->chunk_capacity) {
        uint64_t new_cap = ex->chunk_capacity * 2;
        struct efs_chunk_entry *new = realloc(ex->chunks, new_cap * sizeof(struct efs_chunk_entry));
        if (!new)
            return EFS_ERR_NOMEM;
        ex->chunks = new;
        ex->chunk_capacity = new_cap;
    }
    if (export_ensure_chunk_idx(ex) != 0)
        return EFS_ERR_NOMEM;

    pos = ex->chunk_count++;
    struct efs_chunk_entry *ce = &ex->chunks[pos];
    memset(ce, 0, sizeof(*ce));
    ce->ino = ino;
    ce->chunk_index = chunk_index;
    memcpy(ce->fragment_nodes, fragment_nodes, sizeof(efs_node_id_t) * EFS_NUM_FRAGMENTS);
    memcpy(ce->checksums, checksums, EFS_HASH_SIZE * EFS_NUM_FRAGMENTS);
    chunk_idx_put(ex, ino, chunk_index, pos);
    icnt_inc(ex, ino);
    ex->chunk_epoch++;
    return EFS_OK;
}

int efs_export_get_chunk(struct efs_export *ex, efs_ino_t ino, uint32_t chunk_index,
                         struct efs_chunk_entry *out)
{
    if (!ex)
        return EFS_ERR_INVAL;
    if (export_is_sharded_root(ex)) {
        struct efs_export *tab = efs_export_table_for_chunk(ex, ino,
                                                           chunk_index);
        if (tab && tab != ex)
            return efs_export_get_chunk(tab, ino, chunk_index, out);
    }

    uint64_t pos = 0;
    if (chunk_idx_get(ex, ino, chunk_index, &pos) == 0) {
        if (out)
            *out = ex->chunks[pos];
        return EFS_OK;
    }
    return EFS_ERR_NOT_FOUND;
}

static int grow_inodes(struct efs_export *ex)
{
    uint64_t need = ex->inode_count + 1;
    uint64_t new_cap;
    if (need < 16)
        need = 16;
    if (ex->inode_capacity >= need && (ex->inodes || ex->ino_slabs))
        return EFS_OK;
    new_cap = ex->inode_capacity ? ex->inode_capacity * 2 : 16;
    if (new_cap < need)
        new_cap = need;
    return inode_ensure_cap(ex, new_cap);
}

int efs_export_upsert_inode(struct efs_export *ex, const struct efs_inode *rec)
{
    if (!ex || !rec || rec->ino == 0)
        return EFS_ERR_INVAL;
    if (export_is_sharded_root(ex)) {
        struct efs_export *tab = efs_export_table_for_ino(ex, rec->ino);
        if (tab && tab != ex)
            return efs_export_upsert_inode(tab, rec);
    }
    if (!ex->ino_keys && export_reindex(ex) != 0)
        return EFS_ERR_NOMEM;

    struct efs_inode_mem *cur = inode_ptr(ex, rec->ino);
    if (!cur) {
        if (grow_inodes(ex) != 0)
            return EFS_ERR_NOMEM;
        if (export_ensure_inode_idx(ex) != 0)
            return EFS_ERR_NOMEM;
        uint64_t pos = ex->inode_count++;
        if (inode_from_rpc(ex, inode_at(ex, pos), rec) != 0) {
            ex->inode_count--;
            return EFS_ERR_NOMEM;
        }
        dentry_bytes_add(ex, rec->name);
        idx_put(ex->ino_keys, ex->ino_vals, ex->ino_mask, rec->ino, pos);
        name_idx_put(ex, rec->parent, rec->name, pos);
        child_idx_add(ex, rec->parent, pos);
        return EFS_OK;
    }
    if (cur->parent != rec->parent ||
        (rec->name[0] && strcmp(inamep(ex, cur), rec->name) != 0)) {
        /* Rebind the existing row (rename between base fetch and rebase):
         * same index dance as efs_export_rename. */
        uint64_t slot = inode_slot_of(ex, cur);
        child_idx_del(ex, cur->parent, slot);
        name_idx_del(ex, cur->parent, inamep(ex, cur));
        dentry_bytes_sub(ex, inamep(ex, cur));
        if (inode_from_rpc(ex, cur, rec) != 0)
            return EFS_ERR_NOMEM;
        dentry_bytes_add(ex, rec->name);
        name_idx_put(ex, rec->parent, rec->name, slot);
        child_idx_add(ex, rec->parent, slot);
        sync_hardlink_attrs(ex, rec->ino, cur);
        return EFS_OK;
    }
    if (inode_from_rpc(ex, cur, rec) != 0)
        return EFS_ERR_NOMEM;
    /* Hard-link rows share mode/uid/times; upsert of the indexed row
     * alone would leave the other names with a stale mode. */
    sync_hardlink_attrs(ex, rec->ino, cur);
    return EFS_OK;
}

int efs_export_inode_slot(struct efs_export *ex, efs_ino_t ino, uint64_t *slot)
{
    if (!ex || !slot || ino == 0)
        return EFS_ERR_INVAL;
    uint64_t pos = 0;
    if (ex->ino_keys &&
        idx_get(ex->ino_keys, ex->ino_vals, ex->ino_mask, ino, &pos) == 0 &&
        pos < ex->inode_count && inode_at(ex, pos)->ino == ino) {
        *slot = pos;
        return EFS_OK;
    }
    return EFS_ERR_NOT_FOUND;
}

int efs_export_chunk_slot(struct efs_export *ex, efs_ino_t ino, uint32_t chunk_index,
                          uint64_t *slot)
{
    if (!ex || !slot)
        return EFS_ERR_INVAL;
    uint64_t pos = 0;
    if (chunk_idx_get(ex, ino, chunk_index, &pos) == 0) {
        *slot = pos;
        return EFS_OK;
    }
    return EFS_ERR_NOT_FOUND;
}

int efs_export_needs_chunk_grow(const struct efs_export *ex)
{
    if (!ex)
        return 1;
    if (ex->chunk_count >= ex->chunk_capacity)
        return 1;
    if (!ex->chunk_keys || ex->chunk_count * 2 > ex->chunk_mask + 1)
        return 1;
    return 0;
}

int efs_export_reserve_chunks(struct efs_export *ex, uint64_t extra)
{
    if (!ex)
        return EFS_ERR_INVAL;
    uint64_t need = ex->chunk_count + (extra ? extra : 1);
    while (ex->chunk_capacity < need) {
        uint64_t new_cap = ex->chunk_capacity ? ex->chunk_capacity * 2 : 16;
        if (new_cap < need)
            new_cap = need;
        struct efs_chunk_entry *n = realloc(ex->chunks, new_cap * sizeof(*n));
        if (!n)
            return EFS_ERR_NOMEM;
        ex->chunks = n;
        ex->chunk_capacity = new_cap;
    }
    if (export_ensure_chunk_idx(ex) != 0)
        return EFS_ERR_NOMEM;
    return EFS_OK;
}

#define EFS_FLUSH_HDR_ICOUNT_OFF 276

uint32_t efs_meta_page_count_for_blob(uint32_t blob_len)
{
    if (blob_len == 0)
        return 0;
    return (blob_len + EFS_META_PAGE_SIZE - 1) / EFS_META_PAGE_SIZE;
}

static uint32_t page_count_u64(uint64_t blob_len)
{
    if (blob_len == 0)
        return 0;
    if (blob_len > (uint64_t)UINT32_MAX)
        return UINT32_MAX;
    return efs_meta_page_count_for_blob((uint32_t)blob_len);
}

static size_t dentry_rec_len(const char *name)
{
    return 2 + strnlen(name ? name : "", EFS_MAX_NAME - 1);
}

static void dentry_bytes_add(struct efs_export *ex, const char *name)
{
    if (ex)
        ex->dentry_bytes += dentry_rec_len(name);
}

static void dentry_bytes_sub(struct efs_export *ex, const char *name)
{
    if (!ex)
        return;
    size_t n = dentry_rec_len(name);
    if (ex->dentry_bytes >= n)
        ex->dentry_bytes -= n;
    else
        ex->dentry_bytes = 0;
}

/* Approximate resident bytes of ONE table's cache structures (slab rows,
 * per-slab name arenas, chunk array, open-addressing indexes, child vecs).
 * Used by the client staging-table cap — an occupancy estimate, not a
 * serialization number. Caller holds the relevant locks. */
static uint64_t export_staged_bytes_one(const struct efs_export *ex)
{
    if (!ex)
        return 0;
    uint64_t b = 0;
    if (ex->ino_slabs) {
        for (uint32_t i = 0; i < ex->ino_slab_n; i++) {
            const struct efs_ino_slab *sl = &ex->ino_slabs[i];
            if (sl->rows)
                b += (uint64_t)EFS_INO_SLAB_ROWS * EFS_INODE_ROW_SIZE;
            b += sl->names_cap;
        }
        b += (uint64_t)ex->ino_slab_n * sizeof(struct efs_ino_slab);
    } else if (ex->inodes) {
        b += ex->inode_capacity * sizeof(struct efs_inode_mem);
    }
    b += ex->chunk_capacity * sizeof(struct efs_chunk_entry);
    if (ex->ino_keys)
        b += (ex->ino_mask + 1) * 2 * sizeof(uint64_t);
    if (ex->name_keys)
        b += (ex->name_mask + 1) * 2 * sizeof(uint64_t);
    if (ex->chunk_keys)
        b += (ex->chunk_mask + 1) * 2 * sizeof(uint64_t);
    if (ex->icnt_keys)
        b += (ex->icnt_mask + 1) * (sizeof(uint64_t) + sizeof(uint32_t));
    if (ex->child_keys)
        b += (ex->child_mask + 1) * 2 * sizeof(uint64_t);
    b += ex->child_vec_cap * sizeof(struct efs_child_vec);
    for (uint64_t i = 0; i < ex->child_vec_count; i++)
        b += ex->child_vecs[i].cap * sizeof(uint64_t);
    return b;
}

/* Total staged bytes over the main table and every loaded shard tab. */
uint64_t efs_export_staged_bytes(const struct efs_export *ex)
{
    uint64_t b = export_staged_bytes_one(ex);
    if (ex && ex->shard_tabs) {
        for (uint32_t s = 1; s < ex->shard_tab_cap; s++)
            if (ex->shard_tabs[s])
                b += export_staged_bytes_one(ex->shard_tabs[s]);
    }
    return b;
}

/* Reclaim one tab's over-capacity after mass removal (client staging-cache
 * evictor). Live rows are a dense prefix [0, inode_count) (remove_inode_slot
 * swap-removes), so slabs at/above ceil(count/ROWS) hold only stale bytes.
 * Hash indexes and the chunk array only ever GROW otherwise — without this
 * the RSS floor is the walk's high-water mark, not the cap. */
static void compact_one_tab(struct efs_export *ex)
{
    if (!ex)
        return;
    /* 1. Free tail slabs above the live prefix. */
    if (ex->ino_slabs) {
        uint64_t need = (ex->inode_count + EFS_INO_SLAB_ROWS - 1) /
                        EFS_INO_SLAB_ROWS;
        if (ex->ino_slab_n > need) {
            for (uint64_t si = need; si < ex->ino_slab_n; si++) {
                free(ex->ino_slabs[si].rows);
                free(ex->ino_slabs[si].names);
            }
            if (need == 0) {
                free(ex->ino_slabs);
                ex->ino_slabs = NULL;
                ex->ino_slab_n = 0;
            } else {
                /* Shrink the slab array; on realloc failure keep the old
                 * (larger) block — ino_slab_n bounds every access. */
                struct efs_ino_slab *ns =
                    realloc(ex->ino_slabs, need * sizeof(*ns));
                if (ns)
                    ex->ino_slabs = ns;
                ex->ino_slab_n = (uint32_t)need;
            }
            ex->inode_capacity = (uint64_t)ex->ino_slab_n * EFS_INO_SLAB_ROWS;
        }
    }
    /* 2. Shrink the chunk array when less than 1/4 full. */
    if (ex->chunks && ex->chunk_capacity > 64 &&
        ex->chunk_count * 4 < ex->chunk_capacity) {
        uint64_t nc = ex->chunk_count * 2;
        if (nc < 64)
            nc = 64;
        struct efs_chunk_entry *ncv =
            realloc(ex->chunks, nc * sizeof(*ncv));
        if (ncv) {
            ex->chunks = ncv;
            ex->chunk_capacity = nc;
        }
    }
    /* 3. Rebuild hash indexes whose load factor dropped under 25%.
     * export_reindex_inodes rebuilds ino + name; export_reindex_chunks
     * rebuilds chunk + icnt. Hints key off count/capacity, both already
     * shrunk above. */
    if (ex->ino_keys && ex->inode_count * 4 < ex->ino_mask + 1)
        (void)export_reindex_inodes(ex);
    if (ex->chunk_keys && ex->chunk_count * 4 < ex->chunk_mask + 1)
        (void)export_reindex_chunks(ex);
}

void efs_export_compact(struct efs_export *ex)
{
    if (!ex)
        return;
    compact_one_tab(ex);
    if (ex->shard_tabs) {
        for (uint32_t s = 1; s < ex->shard_tab_cap; s++)
            if (ex->shard_tabs[s])
                compact_one_tab(ex->shard_tabs[s]);
    }
}

void efs_export_meta_page_usage(const struct efs_export *ex,
                                uint32_t *ino_pages, uint32_t *chunk_pages)
{
    uint32_t ip = 0, cp = 0;
    if (ex) {
        ip = page_count_u64(efs_meta_ino_region_bytes(ex->inode_count));
        cp = page_count_u64(ex->chunk_count * (uint64_t)EFS_CHUNK_WIRE_SIZE);
    }
    if (ino_pages)
        *ino_pages = ip;
    if (chunk_pages)
        *chunk_pages = cp;
}

uint32_t efs_export_shard_of(efs_ino_t ino, uint32_t shard_bits)
{
    if (shard_bits == 0)
        return 0;
    /* The root inode and its dentries always live on the main table. */
    if (ino == EFS_ROOT_INO)
        return 0;
    /* Low bits select the shard: shard s owns the congruence class
     * ino == s (mod 2^bits), so every shard has an unbounded ino space.
     * (The old ino >> bits scheme gave each shard only 2^bits inos, so
     * shard 0 exhausted after two creates and every mkdir failed with
     * EEXIST; shard ids were also unbounded, overflowing shard_tabs.) */
    return (uint32_t)((uint64_t)ino & ((1ull << shard_bits) - 1));
}

uint32_t efs_export_chunk_shard_of(efs_ino_t ino, uint32_t chunk_index,
                                   uint32_t shard_bits)
{
    if (shard_bits == 0)
        return 0;
    uint32_t base = efs_export_shard_of(ino, shard_bits);
    uint32_t group = chunk_index >> EFS_CHUNK_GROUP_SHIFT;
    uint32_t mask = (1u << shard_bits) - 1u;
    /* Group 0 stays on the inode shard so a small file's mappings and
     * inode row share an owner (one GETCHUNKS hop). Later groups mix. */
    uint32_t mix = group ? (uint32_t)hash_mix((uint64_t)group) : 0;
    return base ^ (mix & mask);
}

struct efs_export *efs_export_table_for_chunk(struct efs_export *ex,
                                              efs_ino_t ino,
                                              uint32_t chunk_index)
{
    if (!ex)
        return NULL;
    if (!export_is_sharded_root(ex))
        return ex;
    return efs_export_table(ex, efs_export_chunk_shard_of(ino, chunk_index,
                                                         ex->root.shard_bits));
}

uint32_t efs_export_dentry_shard_of(efs_ino_t parent, const char *name,
                                    uint32_t shard_bits)
{
    if (shard_bits == 0 || !name)
        return 0;
    uint64_t h = hash_name_key(parent, name);
    return (uint32_t)(h & ((1ull << shard_bits) - 1));
}

int efs_inode_dir_is_spread(const struct efs_inode *dir)
{
    if (!dir)
        return 0;
    return (dir->imm_files + dir->imm_dirs) >= EFS_DIR_SPREAD_MIN;
}

int efs_export_dir_is_spread(struct efs_export *ex, efs_ino_t dir)
{
    struct efs_inode d;
    if (!ex || !dir)
        return 0;
    if (efs_export_get_inode(ex, dir, &d) != 0)
        return 0;
    return efs_inode_dir_is_spread(&d);
}

efs_node_id_t efs_shard_owner_of(uint32_t shard, uint32_t shard_count,
                                 const efs_node_id_t *live, uint32_t nlive)
{
    if (!live || nlive == 0)
        return 0;
    /* Canonical order: sort ascending. Callers build their live lists in
     * different orders (client: cluster order; server: self first), and
     * live[shard % nlive] on unsorted lists made every node compute a
     * different owner for the same shard — non-shard-0 recs were dropped
     * by everyone and no node flushed the extra shards. */
    efs_node_id_t sorted[EFS_MAX_NODES];
    uint32_t n = 0;
    for (uint32_t i = 0; i < nlive && n < EFS_MAX_NODES; i++) {
        if (live[i] == 0)
            continue;
        uint32_t j = n++;
        while (j > 0 && sorted[j - 1] > live[i]) {
            sorted[j] = sorted[j - 1];
            j--;
        }
        sorted[j] = live[i];
    }
    if (n == 0)
        return 0;
    if (shard_count <= 1)
        return sorted[0]; /* lowest live id = metadata primary */
    return sorted[shard % n];
}

void efs_export_root_free(struct efs_export_root *root)
{
    if (!root)
        return;
    if (root->extra_roots) {
        for (uint32_t i = 0; i < root->extra_shard_count; i++)
            efs_export_root_free(&root->extra_roots[i]);
        free(root->extra_roots);
        root->extra_roots = NULL;
    }
    free(root->extra_shard_ids);
    root->extra_shard_ids = NULL;
    root->extra_shard_count = 0;
    free(root->page_checksums);
    root->page_checksums = NULL;
    free(root->page_cis);
    root->page_cis = NULL;
    root->page_count = 0;
}

int efs_export_root_copy(struct efs_export_root *dst, const struct efs_export_root *src)
{
    if (!dst || !src)
        return EFS_ERR_INVAL;
    efs_export_root_free(dst);
    *dst = *src;
    dst->page_checksums = NULL;
    dst->page_cis = NULL;
    dst->extra_shard_ids = NULL;
    dst->extra_roots = NULL;
    dst->extra_shard_count = 0;
    if (src->page_count > 0 && src->page_checksums) {
        size_t n = (size_t)src->page_count * EFS_NUM_FRAGMENTS * EFS_HASH_SIZE;
        dst->page_checksums = malloc(n);
        if (!dst->page_checksums)
            return EFS_ERR_NOMEM;
        memcpy(dst->page_checksums, src->page_checksums, n);
        if (src->page_cis) {
            size_t cn = (size_t)src->page_count * sizeof(uint32_t);
            dst->page_cis = malloc(cn);
            if (!dst->page_cis) {
                free(dst->page_checksums);
                dst->page_checksums = NULL;
                return EFS_ERR_NOMEM;
            }
            memcpy(dst->page_cis, src->page_cis, cn);
        }
    }
    if (src->extra_shard_count && src->extra_roots && src->extra_shard_ids) {
        dst->extra_shard_ids = malloc((size_t)src->extra_shard_count *
                                      sizeof(uint32_t));
        dst->extra_roots = calloc(src->extra_shard_count,
                                  sizeof(struct efs_export_root));
        if (!dst->extra_shard_ids || !dst->extra_roots) {
            efs_export_root_free(dst);
            return EFS_ERR_NOMEM;
        }
        memcpy(dst->extra_shard_ids, src->extra_shard_ids,
               (size_t)src->extra_shard_count * sizeof(uint32_t));
        dst->extra_shard_count = src->extra_shard_count;
        for (uint32_t i = 0; i < src->extra_shard_count; i++) {
            if (efs_export_root_copy(&dst->extra_roots[i],
                                     &src->extra_roots[i]) != EFS_OK) {
                efs_export_root_free(dst);
                return EFS_ERR_NOMEM;
            }
        }
    }
    return EFS_OK;
}

# Apply visibility: staged, committed, visible

[Architecture](architecture.md) · owners: [`src/meta/meta_apply/`](../../src/meta/meta_apply/README.md),
[`src/server/`](../../src/server/README.md) (`raft_host.c`)

This is the one description of when a metadata row written by a Raft apply
can be seen, and by whom. It binds `host_apply` (`src/server/raft_host.c`),
the KV apply scope (`efs_kv_lsm_apply_begin` / `efs_kv_lsm_apply_end`,
`src/kv/kv_lsm.c`, `include/efs/kv_lsm.h`) and the two derived counters in
`src/meta/meta_apply/gc.c`: the D26 pending-GC watermark (`gc_wm_*`) and the
W154 degraded-mark count (`dg_wm_*`). It describes the code at devel
`9bb50fe79`; it changes nothing.

"Staged" here is the KV apply scope's word. It is not the two-phase
publication stage (`pub_stage.c`, `efs_meta_pub_stage_visible` in
`include/efs/meta_apply.h`), which is a committed row of its own.

## The three states of a metadata row

| State | Meaning | Who can read it |
|---|---|---|
| **staged** | Written into the apply thread's scope, not yet in the committed table | Only the applying thread (`apply_scope` is `__thread`; `lookup`/`merge_scan` in `kv_lsm.c` read `apply_scope.mt` while `apply_staging(l)` holds) |
| **committed** | `efs_kv_lsm_apply_end` wrote the scope's items plus the apply cursor as one atomic WAL record (`lsm_batch`) and released the writers lock | Every reader, including a table scan on another thread (`lsm_scan_from` takes `lsm_rlock`, not the writers lock, and so sees "the old committed table" until then: `kv_lsm.h`) |
| **visible** (to a derived counter) | The counter's delta was noted (`efs_meta_gc_pending_note`, `degraded_note`, both in `gc.c`) | Any thread at once, through `efs_meta_gc_pending_peek` / `efs_meta_degraded_count_peek`, under the counter's leaf mutex |

The note is not tied to the commit. Every note runs on the apply thread
inside the scope, so a counter can show a row that is still only staged,
and an aborted scope (`efs_kv_lsm_apply_end` with a NULL cursor) discards
the rows but not the note.

## The sequence of one apply

Driver: the pump thread (`host_pump`) calls the Raft apply callback
`cfg.apply = host_apply` (`raft_host.c`, set in `attach_replica`), one entry
at a time.

1. **Open.** `host_apply` calls `efs_kv_lsm_apply_begin(h->kv)`. It takes
   `l->writers` exclusively and sets `apply_scope.owner`; other writers wait
   until step 5. Applies are therefore serialized by this lock.
2. **Apply-start hook.** Still in `host_apply`, under the writers lock:
   `efs_meta_gc_pending_apply_start()` (`gc.c`) zeroes `gc_wm_cur` and,
   through `degraded_apply_start`, `dg_wm_cur`. These hold "the inserts
   noted since the latest apply began".
3. **Stage.** `host_apply_dispatch` runs the command. Metadata writes go
   `meta_write_batch` → `meta_write_batch_as` (`rows.c`) → `efs_kv_batch` →
   `lsm_batch`, which, while `apply_staging(l)` holds, puts the items into
   `apply_scope.mt` and returns. Nothing reaches the table.
4. **Note.** On the same thread, inside the scope:
   - GC insert: `gc_queue` (`gc.c`) calls `efs_meta_gc_pending_note(+1)`
     when it queues the record, before the batch is even staged
     (its comment: "at queue time, not batch commit");
     `efs_meta_apply_degraded_clear` notes `+1` for a new GC record before
     its `meta_write_batch`.
   - GC retire: `sweep.c` notes `-1` after its `meta_write_batch`.
   - Degraded mark: `degraded_note(+1)` after `meta_write_batch` in
     `efs_meta_apply_degraded_mark` and `efs_meta_apply_recovery_mark`;
     `-1` after `efs_meta_apply_degraded_clear`'s batch when the mark was found; the publish path (`publish.c`) notes the
     net after `meta_write_batch_as`, skipped for a dry PREPARE.
   Known count: the delta is applied (clamped at 0). Unknown count: a
   positive delta is added to both `*_pos` and `*_cur`; a negative one is
   dropped (`efs_meta_gc_pending_note`, `degraded_note`).
5. **Commit or abort.** `host_apply` calls `efs_kv_lsm_apply_end` with the
   apply cursor when the verdict is `EFS_OK`, else with NULL. With a cursor
   it commits every staged item plus the cursor in one `lsm_batch` call
   (one WAL record); with NULL nothing staged escapes. It then clears the
   scope and releases `l->writers`. Only after this do scans on other
   threads see the rows.

## How a derived counter is derived

Driver: the GC thread (`host_gc_thread`), only for groups this node leads
(`efs_raft_role(...) == EFS_RAFT_LEADER`; a follower skips every pass). In
`host_gc_frag_pass` (GC watermark) and `host_rebuild_pass` (degraded count):

1. `*_peek` returns `-1` (unknown: process start; or
   `efs_meta_gc_pending_invalidate` / `efs_meta_degraded_count_invalidate`
   after a snapshot import or its cancel, `snap_import_cancel` and the
   import completion in `raft_host.c`).
2. `efs_meta_gc_pending_begin` / `efs_meta_degraded_count_begin` sets
   `*_pos = *_cur` while the count is unknown.
3. `efs_kv_scan_prefix` counts the committed rows of the anchor's prefix
   (`gc_count_cb`, `dg_count_cb`) without the writers lock, so it runs
   beside an apply in flight and does not see that apply's staged rows.
4. `efs_meta_gc_pending_derived` / `efs_meta_degraded_count_derived` set
   `count = n + *_pos`, clear `*_pos` and mark the count known.
5. A later pass that scans the whole prefix and finds it empty clamps an
   overshoot with `*_zero_if(anchor, expect)`, which refuses if a note
   raced the scan.

## The rule: each row counted once across a leader change

A row must be counted by the scan or by `*_pos`, never by neither, and
should not be counted by both. Because step 2 of the apply runs under the
writers lock and applies are serialized by it, every note made before the
latest `apply_start` belongs to an apply whose `efs_kv_lsm_apply_end`
already ran: its rows are committed (or were aborted) and the scan counts
the live ones. So `begin` keeps only the latest apply's notes (`*_cur`,
maybe still staged) and the notes made during the scan; derive adds them
to the committed scan. The result may overshoot by those notes (a row the
latest apply committed before the scan read it, or an aborted apply's
note), never miss a row. An overshoot costs bounded extra scans
(`gc.c` comments); it is the only accepted drift.

## Worked example 1: the follower tenure counted twice

Before the begin calls, every insert a replica applied while its count was
unknown stayed in `*_pos`. A follower never derives, so a node that applied
N marks as a follower and was then elected derived `count = scan + N`, and
the scan already held those N rows. Clears brought the count down to N, not
0; `efs_mgmt status` reported `Protection: DEGRADED` with every mark
retired (k37 step 3). Fixed for the degraded count by
`efs_meta_degraded_count_begin` (`85c309a60`, Task DY:
[k37 page](../status/2026-11/k37-degraded-count-overshoot-20261108.md)) and
for the GC watermark by `efs_meta_gc_pending_begin` (`6e7f838aa`, Task EI:
[blind reader page](../status/2026-11/blind-reader-20261109.md), question 2).

## Worked example 2: the apply in flight counted by neither

DY's `begin` set `dg_wm_pos = 0`. Order: the pump thread opens an apply,
stages a degraded mark and notes it (`dg_wm_pos = 1`); the GC thread calls
`begin` (`dg_wm_pos = 0`) and scans the committed table, which lacks the
staged mark; the apply commits. The count is low by that apply's marks
until the next derive, and a pass that then reads `have == 0` returns
without scanning. Task EI added `*_cur` and the apply-start hook for the GC
watermark (`6e7f838aa`); Task EK made the degraded `begin` keep `dg_wm_cur`
(`946095b28`:
[degraded-count begin race](../status/2026-11/degraded-count-begin-race-20261109.md)).
Unit cases: `test_gc_pending_follower_derive` and `test_degraded_marks` in
`tests/test_meta_apply.c` (`./tests/test_meta_apply gc_pending`,
`./tests/test_meta_apply degraded`).

## For a new derived counter

Note inside the apply; keep a per-apply `*_cur` reset by
`efs_meta_gc_pending_apply_start` (extend it, as `degraded_apply_start`
does); while unknown, drop lowers; call a `begin` that keeps `*_cur` just
before the committed scan; derive as scan plus kept notes; derive only on
one thread. A counter that reads committed rows only (no note) needs none
of this.

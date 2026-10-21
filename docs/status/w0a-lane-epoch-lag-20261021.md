# 0a: STALE replay that never converges — lane epoch behind the inode — 2026-10-21

**Fix PASS for the C1 variant of 0a (one inode-lane epoch race class);
orcd-mini acceptance NOTRUN (testing stopped at the user's request).**
Queue row 0a; found by W125 closure test C1
([closure](w125-closure-20261021.md)). Three private node9901 runs of the
C1 workload on fixed builds (about 64 min in total): no `publish-stalled`,
no `pub-stale` refusal, and no write-path EIO. Unfixed: 2 stalled inodes
and 11 EIO in 15 min. A same-length read mismatch seen on the older fixed
build matches the separately fixed `4058d8141` (below).
Whether the older 0a captures (fstor007 SIGINT drain, ior-hard fsync) share
this cause is not verified.

## Cause

GETCHUNKS hands a writer the chunk's lane stamp (`fenced_epoch`) as its
captured authority epoch. The REPORT precheck (`efs_chunk_publish_authority`)
compares it with the inode's `content_epoch`. A truncate fences only the
lanes it sees, so a cross-group lane (odd lane: its shard is on the other
Raft group) could keep an older stamp than the inode:

1. An extending or shrinking truncate whose tail chunk sits on an
   unregistered cross-group lane published the zero tail stub there
   without registering the lane. No later truncate fenced it.
2. A `LANE_FENCE` on a registered lane with no stamp row skipped it, so a
   lane-local publish checked before the truncate created the stamp at the
   old epoch.
3. `ACTIVATE_LANE` was unchecked, and the truncate fenced the
   `active_lanes` it had read earlier. A publish checked before the truncate
   could register the lane after the truncate's read (or after its commit)
   and create the lane under the old epoch.

Once the stamp lagged, every publish on that lane captured the old epoch,
lost the authority check, and the precheck refused it **without a server
log line**. The client's STALE round re-pulled an unchanged row, replayed
and lost again (~280 `report-stale … replayed=1` lines/s) until D27's
`publish-stalled` stopped it: flush EIO, then EIO on appends and reads.
Only a later truncate of the same file could heal it.

Captured with the new precheck log (unfixed build plus diagnostics):
`raft-host: pub-stale why=authority ino=1126412 ci=7 … epoch=2 want=3` — 676
refusals over two inodes, all `why=authority`, every one with the captured
epoch one behind the inode. The stalled rows match the orcd-mini C1 run: a
zero tail stub (`gen=17947454017703708051`, the stub name of chunk 1) plus
one span, or a full image on lane 7.

## Fix

- `ACTIVATE_LANE` carries the inode epoch the publish was checked against
  (`[ino][mask][content_epoch]`). The apply answers STALE once the epoch has
  moved and the lanes are not yet registered, and the REPORT re-pulls.
  Older log forms still apply unchecked.
- The `TRUNCATE` entry carries the `active_lanes` snapshot its fences came
  from (flag bit 2, bytes 34–41). The apply answers BUSY if a lane was
  registered since. The client retries, and the retry fences the new lane.
- `LANE_FENCE` on a lane with no stamp writes the stamp at the new epoch.
- The truncate registers an unregistered cross-group tail lane before its
  fences, so the stub lane is fenced like any other.
- Diagnostics: the REPORT precheck logs its silent STALE refusals
  (`pub-stale why=authority|span-base|span-norow|image-base`, at most
  20 lines/s). The client's `publish-stalled` line names the rec's base,
  span and captured epoch.

Regression: `tests/test_meta_apply.c` `test_lane_epoch_never_lags` covers
items 2 and 3. It fails 6 checks without the apply changes and passes with
them. Item 1 is host-level and is covered by the live reproducer.

## Environment and builds

- node9901 (orcd-mini daemon node). Private 3-daemon cluster:
  `w125_p0_cluster.sh`, RDMA `mlx5_0`, `--direct-io`, quota 200 GiB, stores
  under `/tmp`. Ports 17801–17803; orcd-mini and w100 untouched.
- Two efs-fuse mounts on the same host stand in for the two C1 clients
  (A = `DIR/mnt`, B = `DIR/mntb`).
- Source: devel `6ffa9e785f57` (src/include identical to `287b2b3c4`).
  - Unfixed + diagnostics: build ID `w0a-diag1`.
  - Fixed on that base: this commit's source diff, build ID `w0a-fix2`.
  - Fixed on current devel: release `4058d81416a0` plus
    `git diff 4058d8141 <this commit>`, build ID `w0a-fix3`. Its
    src/include match this commit file for file (md5).
  - Each build is a copy of the release dir built with its `mk.sh`
    (libfuse at `/data1/efs/deps/libfuse`).

## Reproduction

```
scripts/w0a_lane_lag_repro.sh REL DIR [PORT=17800] [SECONDS=1500]
```

Run on node9901 in a named screen. REL is a built release dir, DIR is under
`/tmp`. The script starts the cluster and mounts B, then runs
`w125_concurrent.py` for SECONDS:

- A: 4 writers, 4 readers, 2 deleters, 4 modifiers.
- B: `--remote`, 4 readers, 2 deleters.

It then collects `DIR/out/{a,b,stalled,pubstale}.txt` and stops the cluster.
**PASS:** no `publish-stalled` or `flush-meta` lines and `errors=0` on both
clients. Cleanup: `rm -rf DIR`.

## Results

| Run | Build | Time | A | B | Stalled | `pub-stale` |
| --- | --- | --- | --- | --- | --- | --- |
| orcd-mini C1 rerun (W125 session) | `6ffa9e785f57` | 3600 s | errors=31, mismatch=4 | — | 5 inodes | (no log) |
| private, unfixed | `w0a-diag1` | 900 s | errors=11, mismatch=0 | mismatch=6 | 2 inodes | 676, all `authority` |
| private, fixed run 1 | `w0a-fix2` | 1500 s | errors=0, mismatch=1 | mismatch=1 | 0 | 0 |
| private, fixed run 2 | `w0a-fix2` | 1282 s (stopped) | errors=0, mismatch=1 | mismatch=0 | 0 | 0 |
| private, devel + fix | `w0a-fix3` | 1071 s (stopped) | errors=22 (rename BUSY), mismatch=0 | errors=0, mismatch=0 | 0 | 0 |

- Unfixed B's 6 mismatches are all one file, `w1/d024/f013724`, whose v3
  rewrite was the stalled publish.
- An intermediate fix (skip the stub instead of registering its lane) also
  ran clean of stalls for 1500 s. It was replaced because, by code
  reading, the stub's CAS is what sends a pre-truncate image through a
  replay merge; skipping it would drop that.
- `report-stale` lines: unfixed 1932 in 900 s, fixed 15 in 1500 s.
- The last two runs were stopped early at the user's request, so they have
  no final verification pass.
- The 22 `w0a-fix3` errors are all writer `rename(.tmp → name)` calls that
  failed BUSY (`rename_at … rc=-13 after 2 tries`) within 10 s around
  13:24Z.
  - At that time group 0 elected a new leader (term 1 → 2), and REPORT
    activation answered NOT_PRIMARY.
  - node9901 load average was about 31 with two private clusters running.
  - No publish failed. This is metadata availability during an election,
    not 0a, and it is not investigated here.

## Same-length read mismatch (fixed separately in `4058d8141`)

The `6ffa9e785` fixed runs and the unfixed orcd-mini C1 rerun show reads of
the right length with wrong content. The `w0a-fix3` run on devel, which
includes `4058d8141`, shows none. `4058d8141` fixes a small write after
chunk-map eviction that published zeros plus the write as the whole chunk.
That matches this pattern, but the match is not proven.

- **Files:** `w3/d039/f013889` (shrink into chunk 17, append), `f009830`
  (shrink into chunk 4, append); on orcd-mini `f012713`, `f041061`,
  `f010522`.
- **Symptom:** both clients read it, then a later version reads correctly
  (`verified_bad=0`). On `f013889` the client logged `read hole … ci=17`
  for the tail chunk: GETCHUNKS returned no row for a chunk that holds
  data.
- **Status:** not 0a. Treat it as the `4058d8141` defect, which was
  fixed before this commit (its reproducer is `scripts/w125_evict_rmw.py`).
  It stays open if it recurs on a build that includes `4058d8141`.

## Limitations and remaining gates

- **Coverage:**
  - Private cluster with both clients on one host.
  - Not rerun on orcd-mini, which was not to be restarted during the
    W125 closure tests.
  - **NOTRUN, stopped at the user's request:** the C1 rerun on orcd-mini
    after deployment, which is the acceptance gate, and a full-length
    fixed run with final verification.
- **Truncate BUSY window:** a truncate refused with BUSY after its
  cross-group fences have applied leaves those lanes one epoch ahead of the
  inode until the client's retry commits. This predates the fix (open append
  reservations already cause it). Publishes on those lanes STALE in the
  meantime. If every retry fails, the lanes stay ahead.
- **Retained logs:** node9901 `/tmp/w0a/` holds:
  - `diag-logs/` (unfixed);
  - `clfix1/logs` (intermediate);
  - `f2run{1,2}/logs` and `f3run1/logs` (fixed).
  - The stores were deleted.

  `/tmp` is not durable, so the summary above is the record.
- **`make test`:** NOTRUN (stopped at the user's request). Run on node9901:
  - `test_meta_apply`, `test_sim`, `test_append_transaction`,
    `test_fence_view`, `test_raft`, `test_txn`;
  - `test_lane_bootstrap_recovery`;
  - `test_publication_{recovery,session}`, `test_mtime_barrier`.

  All PASS on `w0a-fix2`. `test_meta_apply` and `test_sim` also PASS on
  `w0a-fix3`.

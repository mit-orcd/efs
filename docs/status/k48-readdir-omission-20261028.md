# Task S — K48 intra-call directory migration omission

`agent/devel-task-s`, base `b2d57e421`: qualification NOTRUN/pending. Evidence `docs/status/k48-readdir-omission-20261028.md`; intel1 `~/efs-artifacts/taskS-20261028/`. Deterministic unchanged-base loopback FAIL 95/96 (`a00007` missing); fixed probe PASS 96/96. No release PASS claimed; ten-run regressions and full gate pending. Original 677-file evidence verified unchanged.

## Forensic result and limits

The user requested Task S from the parallel-task assignment. Intel1 is idle
for private work; its production mount/account is untouched. Work uses
`~/efs-taskS`, free loopback ports and fresh synthetic fixtures only.
Original Task N stores remain read-only at
`/home/maik/efs-taskN-20261027/open-w130-initial/efs-w130-7jltagai`.
All **677 files** still match `open-w130-initial-files.json` from Task N.

A reader compiled against the exact original source `f98578e697d3` opens
**copies** of each W130 KV, never the originals. This is forensic inspection,
not backward-compatibility support or an upgrade test. The original KV
format is read by its own original implementation. Reproduction utility:
`scripts/forensics/k48_store_scan.c`, linked to that source's `libefs.a`.
Opening the LSM can recover/truncate a WAL, so using copies is mandatory.
Target directory inode **2205**, group 0:

| Replica | Physical dentry rows / live names | Static READDIR |
| --- | --- | --- |
| Healthy node 1 | 96 / 96; all f0000–f0095 present | All 96 at page sizes 1, 2, 4, 8, 16, 32, 64 and 128 |
| Healthy node 2 | 96 / 96; all f0000–f0095 present | Same |
| Halted node 3 | 95 / 80 (includes tombstones); partial group state | 52 at every tested page size |

Both healthy rows are SPLITTING with bitmap `3ff7dffedbfdfbff`; the halted
row has only `0000000200000200`. No permanent loss is observed on the healthy
quorum. Static pagination alone does not omit a name. **The original
failure recorded only 95, not the returned name set or serving RPC/cursor
trace. Its exact missing filename/interleaving cannot be recovered from the
retained final stores.** Later readback does not prove a precise historical
ReadIndex/apply ordering. The new controlled reproduction pins the listing
race below; it does not invent that missing historical trace or relabel the
original failed gate.

Graphify mapped the existing frontier/readdir code; source inspection and
executable negative controls establish the finding, not graph edges.

## Proven cause and correction

READDIR reads the inode's layout/used-lane bitmap, then scans sources using
separate KV operations. Migration can move a LOCAL name after that row cut.
The captured bitmap can omit its newly populated hashed lane. A page cursor
can pass the lane, so later pages never visit it even though the entry exists.
The LOCAL→SPLITTING transition has the related stale source-count problem.

The deterministic unit creates 96 names, places later names on lane 63 and
moves `a00007` to lane 1 between inode read and scan. On unchanged product
code, a 64-entry page and the next page total **95**, missing `a00007`.
A 128-entry call also fails; a LOCAL→SPLITTING variant fails too. All three
failures have rc=OK: this is silent omission, not a decoder refusal.

`efs_meta_apply_readdir` now rechecks layout, layout epoch and used lanes
before publishing the page/cookie. Structural change discards that page and
rescans from the original cookie. The bitmap gains at most 64 bits and the
layout advances twice, bounding legitimate migration retries. Error/refusal
paths remain errors; no missing entry is accepted. This is not a snapshot
across separate page RPCs; existing client name deduplication remains.

The server retains this RPC's ReadIndex proof mask. If its final row shows
a newly needed group, it fences that group and rescans from the request's
original cursor; a KV retry cannot manufacture Raft authority. No proof
progress refuses rather than looping/serving. Tests execute the production
callback with late-group, failed-proof and missing-proof negative controls.
No format, wire, durability, placement or deadline changes.

## Deterministic loopback evidence

`tests/test_k48_readdir.py` owns three fault-built daemons and one FUSE
client. It holds background DIR migration with the existing serve-test hook,
creates 64 LOCAL files, begins splitting, damages node 3 and halts its group
with another DIR apply, then fsyncs 32 more hashed files. A new fault-only
bounded pause (`EFS_FAULT_READDIR_LAYOUT_FILE`) pins READDIR after its row
cut. The test issues a normal consensus `raft-dir migrate`, releases the
pause and requires all 96 names and payloads with no application retry.
It then requires eight more exact listings and checked clean client stop.
The pause changes no metadata and is absent from normal efsd.

- Unchanged `b2d57e421` plus identical test pause/build plumbing:
  **FAIL 95/96, missing `a00007`**, no extras, clean stop rc 0.
- Fixed probe: **PASS 96/96**, exact payloads/repeated listings/clean stop,
  **11.91 s**. This exploratory build is not the frozen qualification.
- Unit baseline: three intended omission failures; fixed unit PASS.
- First unit attempt exceeded the old helper's 64-name/16-page capacity;
  corrected helper capacity is 128. That setup failure is retained separately.
- First live attempt omitted the metadata fault object from the fault link
  and had a wrong client-stop path; second attempt addressed management to
  the seed rather than the group's leader. Both are retained harness failures,
  not valid product negative controls. Corrected fixture selects a healthy
  group-0 leader and routes management commands to that leader.

The new test is in `make test-long`, 15 s / two slots, derived from
1.25×11.91 s rounded up to 5 s. Timeouts/oracles and D44 target remain unchanged.
Qualification uses `scripts/task_s_qualify.py FULL_COMMIT ARTIFACT_ROOT`:
unit/authority checks, three deterministic live runs, W130 and K33 payload
ten times each serially/alone, then the eight-job full code gate. It stops
on the first failure, retains evidence and never retries it into PASS.

## Pending acceptance and retention

K48 remains OPEN pending frozen-source qualification and review of the
historical-causality limit above. Exact tested identity, commands, counts,
all failures, host/RDMA scope, final raw SHA256SUMS and cleanup receipts will
be added after the controller closes. No original Task N stores are pruned.
Own forensic copies/failed fixtures stay only while this investigation is
open. Raw logs, copied runtime data and profiler/graph outputs are not Git.

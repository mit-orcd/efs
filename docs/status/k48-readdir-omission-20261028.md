# Task S — K48 intra-call directory migration omission

`agent/devel-task-s`, base `b2d57e421`, tested candidate **`947edd15f3ecb6b0b95c2acafe9eb62a241ce910`**: reproduced listing race **FIXED, TCP-loopback qualification PASS** (3/3 deterministic, W130 10/10, K33 payload 10/10). Full gate **523/0 FAIL/12 NOTRUN, INCOMPLETE, 679 s**. K48 remains **OPEN, blocks alpha: yes**, specifically because the original omitted name/RPC interleaving was never captured and historical attribution remains unproven; the demonstrated intra-call defect is corrected. Original 677-file evidence verified unchanged before and after cleanup. No push or merge.

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
row has only `0000000200000200`. No permanent dentry loss is observed on the healthy
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

## Frozen qualification results

Candidate `947edd15f3ecb6b0b95c2acafe9eb62a241ce910` (product fix
`3426111b4`), source `intel1:~/efs-taskS/qualification-v2/src`, detached
screen `taskS-qualification-v2`. Output:
`/home/maik/efs-artifacts/taskS-20261028/qualification-947edd15f/`.
Three deterministic cases PASS (12.545/10.791/11.241 s), unit and
server-authority controls PASS. W130 **10/10 PASS**, including complete
listings through the healthy quorum, frozen halted-group indices and
successful snapshot resync. K33 payload **10/10 PASS** across 3→4→5
additions, concurrent writer, packing, orderly restart and single-member
outages. No application retry accepts an error. The eight-job full code
gate completed with **0 failures**, with the host limits below.

The first frozen candidate `cdf67d98f` passed its first deterministic test
but stopped on fixture leader setup in its second: un-targeted step-down
re-elected node 3, so the healthy-leader premise was not established before
fault arming. This is retained as a setup failure, not an omission or a
qualification PASS. The fixture now uses the existing targeted
`place_leaders` transfer, with the same 15 s setup bound; it does not retry
an application outcome. Production code is identical between the two
qualification freezes.

The built-source recipe model is **1281 s** (pool plus exclusive tail),
versus **1277 s** with the K48 command omitted; the new test adds 4 modeled
seconds. Both exceed D44's **1100 s target**, while remaining under its
existing **1650 s bound**. These are scheduling estimates, not observed starts
or wall time. No target/slot/deadline was relaxed; the required regression's
15 s/two-slot budget has its measured reason. Per-host budget work belongs
to Task T. The actual final gate wall will be recorded separately.

## Final verdict and retention

The demonstrated listing-path defect is fixed: unchanged-base controls
fail with the exact 95/96 signature, while the frozen candidate passes
the deterministic test and all required repeated regressions. **K48's
original incident remains precisely OPEN**: retained healthy KVs prove
all 96 dentries exist, but the original count-only capture cannot identify
its missing name, serving replica, cursor or exact interleaving. The new
reproduction is evidence of a real same-signature defect, not proof that
it was the only possible cause of the original failure. No missing
payload or permanent healthy-quorum dentry loss is established. The
coordinator can review that attribution limit separately from this fix;
this page does not silently close the alpha blocker.

Raw evidence is closed at
**`intel1:/home/maik/efs-artifacts/taskS-20261028/`**, 330 retained files,
verified recursive `SHA256SUMS` hash:
`57157f9dbdca2b6bc8a044a49ce2641d7b41c58d137fa2826da5e184a0322821`.
It includes original per-replica scans, every baseline/harness failure,
qualification logs/results/version, full summary/timings, source manifest
and final verification, 141 copied fixture logs, and cleanup receipts.
The manifest is computed after all controllers finish and checked against
every retained file. Original Task N stores remain read-only at the path
above, with all 677 files reverified against Task N's full-file manifest
after cleanup.

`PRUNED.txt` records removal via `~/bin/edelete --force --delete` of own
inactive `~/efs-taskS` source/build/KV copies and six explicitly inventoried
synthetic `/tmp` fixtures, plus four own archive files. Finalized text logs
remain; copied runtime stores are reproducible from the method. No Task N/F
tree, production cluster or other agent's work was removed. Final checks
found no private maik efsd/efs-fuse processes or Task S mounts.

## Full eight-job gate and remaining coverage

`TOTAL 523 FAIL 0 NOTRUN 12 STATUS INCOMPLETE (679s) OVERBUDGET 4 BUILD 0s`;
controller wall **680.029 s**, exit **3** (host-limited INCOMPLETE). This is
not an all-environment PASS. The prebuilt gate contains the new K48 test.
The earlier `make -j8 test-bins` took 30.495 s; build time is not concealed
in the gate's reported zero rebuild time.

The 12 NOTRUN commands/subcases are: absent cluster stop/remount checkout
(one), missing `valgrind/memcheck.h` (one), no usable IB device for
declaration/RDMA transport/seven segmented modes (nine), and the
normal-build follower-bound stalled-compactor subcase requiring a fault
build (one). Detailed command lines and reasons are in `code-gate/summary.txt`.
No new RDMA, distinct-host, production-scale or ORCD result is claimed;
those remain NOTRUN for the coordinating release gates.

Four measured commands exceeded their existing budgets, without test
failure or deadline/slot relaxation:

| Command | Measured / budget seconds |
| --- | --- |
| `test_k43_coordinator_kill.py` | 54.2 / 25 |
| `EFS_QUERY_DELTA_DIRS=1 test_query_delta` | 11.4 / 10 |
| `test_k22_leader_admission.py` | 44.7 / 30 |
| `test_kv_lsm` | 10.2 / 10 |

The observed 679 s gate wall is separate from the 1281 s scheduling model
and its 1100 s target overrun described above. Task S changes only the
new regression's measured budget; the four pre-existing fits are not
raised to hide these observations.

Standalone measured walls, seconds (controller wall, including cleanup):

| Check | Serial runs | Verdict |
| --- | --- | --- |
| Deterministic K48 | 12.545, 10.791, 11.241 | 3/3 PASS |
| W130 halt/resync | 29.687, 31.191, 25.176, 21.966, 31.539, 20.662, 26.475, 28.779, 30.687, 25.375 | 10/10 PASS |
| K33 payload | 29.933, 28.480, 30.082, 28.485, 29.083, 27.830, 29.484, 20.011, 25.874, 24.274 | 10/10 PASS |

Payload fixtures end with 104/101/105/105/105/103/104/99/102/99 exact
files respectively (1,027 across independent fixtures), each with 56
small packed files and the remainder large/unpacked. Concurrent writers
completed 24/21/25/25/25/23/24/19/22/19 files, zero retried writes. Every
fixture reports packed_total 56; this is full fixture payload checking
through its prescribed lifecycle, not sampled verification.

## Qualification environment and commands

Intel1: Intel Core Ultra 7 256V, eight CPUs, x86_64, Linux
`6.12.0-211.67.1.el10_2.x86_64`. Binary identity:
`efsd 947edd15f3ecb6b0b95c2acafe9eb62a241ce910 (branch agent/devel-task-s, built 2026-10-28T16:07:12Z on intel1)`.
The isolated source archive is built with `make -j8 test-bins`,
`EFS_FUSE_PREFIX=/usr`. FUSE access and `fusermount3` are prerequisites.
The shared production account, mount, stores and ports are untouched.

Three-member TCP loopback fixtures use free ports, O_DIRECT fragment I/O,
1 GiB per member and normal packing; K33 adds members four and five and
sets packing age/interval to 500/200 ms. The K48 fixture contains 96
32-byte deterministic payloads (64 LOCAL, then 32 HASHED), a 64-entry page
boundary and one deliberately halted replica group. W130 uses its
unchanged 96-file fixture and serve-first subcase. These are synthetic
small fixtures, not production-scale, RDMA or distinct-host qualification.
Expiry and NFS re-export are outside this alpha surface.

Run the committed controller from a fresh exact-commit archive below
`intel1:~/efs-taskS/` with no private test processes/mounts active:

```sh
python3 scripts/task_s_qualify.py \
  947edd15f3ecb6b0b95c2acafe9eb62a241ce910 \
  /home/maik/efs-artifacts/taskS-20261028/UNIQUE_OUTPUT
```

It retains every standalone result, checks normal/fault build hook scope,
and uses `EFS_TEST_TARGET=test-all EFS_TEST_JOBS=8` with
`scripts/make_test_summary.sh`. Its per-case 900 s outer watchdog does not
replace or extend any test's own deadlines; the full-gate watchdog is
3600 s. The first failure stops this qualification, with failed fixture
retention; successful W130 logs are copied after shutdown before cleanup.
The fixture and authority tests themselves are the reproduction source;
no private raw capture is required to reproduce the new omission.

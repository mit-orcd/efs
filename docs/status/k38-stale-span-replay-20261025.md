# K38 · W161 · a STALE span re-sent on its old base until EIO (Oct 25, 2026)

**Purpose.** Checklist item K38 in
[alpha-1.0-2plus1-checklist.md](alpha-1.0-2plus1-checklist.md) (section K),
work item W161 in [work-queue.md](work-queue.md). On orcd-mini, ior hard
(8 ranks on one client, one shared file, 47008-byte transfers) got `EIO`
from `write()` once in four runs on a healthy cluster
([investigation](orcd-mini-deploy-5a7463440-20261025.md#investigation-oct-25-from-the-retained-logs-and-devel-source)):
one span record was re-sent on a stale CAS base every REPORT round, its K32
unit lost 215 other chunks each round, and after the 8 s budget
`report-resolve` turned the PARTIAL receipt into an error that stayed with
the inode. Alpha 1.0 requires that an application write never gets `EIO` on
a healthy cluster and that an acknowledged write is never lost.

**Verdict: PASS (scoped: unit proof and loopback regression evidence).**
intel2 `make test-all` at `bac34da00` (branch `claude/vibrant-wiles-325684`
on devel `5af760ff1`, not merged, not pushed): 483 commands, 0 FAIL, 16
NOTRUN host limits (RDMA device, valgrind, cluster scripts, EFS_FAULTS unit
build), 483 s at `EFS_TEST_JOBS=8`. The proof test fails on the base source
and passes on the branch. The loopback test (24 writers sharing chunks on
one client under dcache pressure) fails 6 of 6 runs on the base and passes
20 of 20 on the branch.
The orcd-mini ior hard rerun (RDMA, the real-host proof) is **NOTRUN**, for
the parent session. Single-host loopback daemons are regression evidence,
not qualification.

## The proof (step 1)

`tests/test_k38_stale_span_replay.py` runs the production putid table, STALE
classifier (`stale_repull_replay`), STALE replay
(`dcache_replay_stale_ex_budgeted`) and REPORT record builder
(`report_fill_owned`) from `src/client/write.c`; fetch, PUT, pull and the
slot lookup are adapters. State: chunk 2033 of inode 4642, span A's putid
present (`span_base_gen` = `c289…`), the slot's body gone, the committed
generation `244b…`. One REPORT round after a STALE reply.

- **On the base source** (devel `5af760ff1`, `K38_WRITE_SOURCE=` the
  pre-fix `write.c`) the test fails with the livelock signature:
  `K38 superseded: the rebuilt REPORT re-sends span a5+400+300 on base
  c28987f070b5f01a against committed 244b546dfa67ace7 (replay returned
  without a body; livelock)`. The hypothesis holds: the body-less replay
  returns `EFS_OK`, the putid keeps the old base, the next round rebuilds the
  identical record.
- **How the state arises.** Two PUTs of one chunk can be in flight at once
  (the flush drops its locks around the PUT). When the older snapshot's PUT
  lands after the newer snapshot landed, was reported and dropped its putid,
  `putid_note` inserted the older span again (the slot already refused it as
  `put-SKIP-old`), with the base the newer commit had superseded. The newer
  commit had also released the body (D23). The same body-less state follows
  from any path that frees the body while a span identity survives.

## Decision: rebuild, fail closed only when the bytes are gone (step 2)

The contract (alpha 1.0, D27 writeback errors): an acknowledged write must
not be lost; a writeback that cannot complete is reported (`EIO` on
`write`/`fsync`/`close`, sticky per inode), never dropped silently. So the
body-less replay never returns `EFS_OK` leaving the old base; it takes the
first case that applies:

| Case | Why it is safe | Action |
| --- | --- | --- |
| A newer snapshot of the chunk landed or committed (`max(object_seq, committed_seq)` of the slot is above the span's seq) | Snapshots are cumulative: the slot keeps its ranges until a commit, so the newer snapshot carries the span's bytes | drop the older identity (`report-replay … superseded by seq=…`); the record is not rebuilt |
| Otherwise | The span's own object landed and is immutable; it holds the acknowledged bytes | rebuild: fetch the committed image, paint the span from its own object (`efs_client_overlay_span`), PUT it as a span on the committed generation with a new snapshot seq; the putid then names the new base |
| The span's own object is unreadable as named (DECODE, CHECKSUM, NOT_FOUND, INVAL; not a NET/BUSY fragment) | The bytes cannot be recovered | fail closed: `report-span-lost …` names the identity, the inode gets a writeback error (`write` refuses at once, `fsync`/`close` return `EIO`), the identity is released (putid and slot), so the rest of the K32 unit commits |

A transient failure (fetch or fragment NET/BUSY) is retained for the next
round, as a body replay's failure is. Fail-closed alone was rejected: in the
reachable case the bytes are not lost (they are in a newer snapshot or in
the span's own object), so failing would turn a recoverable state into data
loss. The late landing is also stopped at its source: `putid_note` refuses a
PUT older than the slot's newest landed or committed snapshot.

## The bound and the resolve message (steps 3, 4)

- **Bound.** The REPORT loop notes every STALE round's records
  (`report_streak_note`). A rebuilt record that two consecutive STALE rounds
  already sent unchanged, on a CAS base other than the chunk's committed
  generation (it can only lose again, and takes its unit with it), is not
  sent a third time: `report-stuck ino=… ci=… gen=… base=… committed=…
  span=… stale_rounds=2: not re-sent, writeback error`; the inode gets the
  writeback error and the identity is released. A record on the committed
  base lost with its unit, not on its own, and stays; so do UNCOND images
  and records a replay changed. `report-loop` lines carry `stuck=N`.
- **Resolve.** The server's receipt keeps the first record an attempt lost
  on its CAS base (the precheck `host_pub_stale_note`, which is
  rate-limited in the log); the receipt query reply carries it (wire change
  to `efs_msg_report_receipt_reply`, no compatibility), and a PARTIAL
  verdict's `report-resolve` line names it: `outcome=partial rc=-14 …
  stuck=<ino>:<ci> gen=<object sent> span=<off>+<len> base=<record base>
  committed=<host generation> why=<check>`, or `stuck=unrecorded (an
  apply-time CAS loser)`.

## Two more losses the loopback test found (same code path, pre-existing)

The required loopback test (step 5) found two silent losses of acknowledged
writes on the base and on the first fix; both are fixed here, each with its
own unit test and negative control. Traces: intel2
`~/efs-artifacts/k38-20261025/install-order-trace/`,
`…/series/src-9-logs/` (untraced), the traced capture below.

1. **Install order** (`dcache_install_image`). Snapshot 641 copied; a write
   patched a record into the body; snapshot 646 copied it and cleared
   `dirty`; PUT 641 landed and installed its older image over the body
   (ranges were overlaid only while dirty), wiping the record until PUT 646
   installed. A STALE replay that copied the body in that window published
   the chunk without it (chunk 539, 47008 zero bytes on both mounts). Fix:
   an image older than the slot's newest snapshot keeps the body's ranges.
   `tests/test_k38_install_order.py` (fails on the base source).
2. **Classifier identity** (`stale_repull_replay`). The classifier pulled the
   host's rows, then read the local table. A newer PUT of the chunk landed in
   between and set the table's generation to its own unpublished object; the
   chunk read as committed, its identity was dropped and its body freed, and
   the host kept the older image (chunk 1159, two writers' records zero; the
   trace shows `stale-class committed=d924… ours=d924… done=1` in the same
   millisecond as `put-record obj=d924…`, and no REPORT of `d924…` after).
   Fix: a chunk is done only when the host holds the object the round sent
   (the sent record's generation and span, and its snapshot seq); a chunk the
   round did not send is left to the rebuild. `tests/test_stale_recovery.py`
   gains the cases.

## Changes

| File | Change |
| --- | --- |
| `src/client/write.c` | `replay_span_without_body` (the three cases), `dcache_identity_abandon`, `dcache_object_seq_of` and the `putid_note` guard; the streak bound in `report_dirty_ino_run`; `dcache_install_image` keeps newer ranges; `stale_repull_replay` classifies against the sent records |
| `src/client/read.c`, `client_internal.h` | `efs_client_overlay_span`: one span from its own object, `EFS_ERR_NET` when a fragment was unreachable (kept apart from DECODE) |
| `src/client/inode_rpc.c` | `report-resolve` names the stuck record of a PARTIAL receipt |
| `include/efs/request_receipt.h`, `src/common/request_receipt.c`, `src/server/raft_host.c`, `include/efs/protocol.h` | receipts keep the first CAS loser; the query reply carries it |
| tests | `test_k38_stale_span_replay.py`, `test_k38_report_bound.py`, `test_k38_install_order.py` (`make test`); `test_k38_shared_chunk_pressure.py` (`make test-long`, `tests/budgets.tsv` 90 s, 2 slots); adapters in `test_report_span_base.py`, `test_fold_observation.py`, `test_get_integrity.py`, `test_stale_recovery.py`, `test_w155_receipt_table.py`, `test_w155_report_resolve.py` |

No new `EFS_*` reader (the loopback test sets the existing
`EFS_DCACHE_HARD_BYTES` and `EFS_DCACHE_BYTES`), no KV kind, Raft tag or
message type; one wire struct grows (receipt query reply).

## Environment

intel2 (Rocky Linux, 8 CPUs, `/home` XFS), private tree `~/k38-work/src`
copied with `git archive`, build `make all tests/faults/efsd` (gcc, `-O3`,
the EFS_FAULTS daemon for loopback tests), `TMPDIR=~/k38-work/tmp`. Loopback
daemons: three `tests/faults/efsd` members on free 127.0.0.1 ports, 1 GiB
quota each, two efs-fuse mounts (TCP, buffered I/O), no RDMA. The base for
comparisons: devel `5af760ff1` built the same way in `~/k38-work/base`.

## Results

All on intel2. Commits: `48c78447a` (K38 replay, bound, resolve, tests),
`554f6e16d` (install order), `bac34da00` (classifier identity); the gate and
the last loopback series ran on `bac34da00`.

| Check | Base `5af760ff1` | Branch |
| --- | --- | --- |
| `test_k38_stale_span_replay.py` (proof) | FAIL, livelock signature (above) | PASS: superseded drop and late-PUT guard, rebuild on `244b…` with the span's bytes from its own object, fail-closed lost object (writeback error, identity released, REPORT goes on), transient retained |
| `test_k38_install_order.py` | FAIL (`PUT 641's image erased byte 10 written after its copy`) | PASS |
| `test_k38_report_bound.py`, `test_stale_recovery.py`, `test_w155_receipt_table.py`, `test_w155_report_resolve.py` | — (new cases) | PASS; resolve prints `outcome=partial rc=-14 … stuck=<ino>:1 gen=… span=… base=5 committed=4d why=span-base` |
| `make test` | — | PASS |
| Loopback `test_k38_shared_chunk_pressure.py` (24 writers on one client, 200 records each, 47008 B, one fsync at the end, 32 MiB body budget) | 6 of 6 runs FAIL: 5 `EIO` (one record STALE 26–32 rounds in a row, `outcome=partial`, a span or a full image on a moved base), 1 with lost records | after the first fix (`48c78447a`+`554f6e16d`): 9 of 10 PASS, 1 lost records (the classifier race, traced); final (`bac34da00`): **20 of 20 PASS**, 8–20 `dcache-pressure` lines and 1–18 STALE replay rounds per run, 22–36 s each |
| Loopback, 8 writers on one client, 400 records (earlier shape, no pressure check) | 2 of 12 runs lost a whole record, 1 `EIO` | (after `48c78447a` only) 2 of 28 lost a record, 0 `EIO`; the traced one is the install-order loss |
| Loopback `--clients 2` (16 writers, 240 records) | 3 of 7 `EIO` (unit BUSY) | 2 of 3 `EIO` (unit BUSY): the separate finding below |
| `make test-all` (`scripts/make_test_summary.sh`, `EFS_TEST_JOBS=8`, fresh tree at `bac34da00`) | — | run 2: **483 commands, 0 FAIL, 16 NOTRUN** host limits, 483 s (OVERBUDGET: `test_k36_segment_recovery.py` 127 s of 100, untouched). Run 1: 1 FAIL, `test_k36_recovery.py` step 8 read `EIO` after the compaction crash; 3 of 3 PASS alone on the branch and on the base, known intermittent under load; the new loopback test PASS in both runs |


## Reproduce

```bash
make all tests/faults/efsd
python3 tests/test_k38_stale_span_replay.py
python3 tests/test_k38_report_bound.py
python3 tests/test_k38_install_order.py
python3 tests/test_stale_recovery.py
python3 tests/test_w155_report_resolve.py
python3 tests/test_k38_shared_chunk_pressure.py
EFS_TEST_JOBS=8 scripts/make_test_summary.sh
```

Negative controls: `git show 5af760ff1:src/client/write.c > /tmp/w.c`, then
`K38_WRITE_SOURCE=/tmp/w.c python3 tests/test_k38_stale_span_replay.py` and
the same for `tests/test_k38_install_order.py` (both must fail). Loopback
comparison: build devel `5af760ff1` in a second tree, copy
`tests/test_k38_shared_chunk_pressure.py` into it and run it there in a loop
(`EFS_K38_KEEP=1` keeps a failed run's stores and logs; `EFS_DCACHE_TRACE=1`
traces the client's slots). The test needs `/dev/fuse`, `fusermount3` and
`efs-fuse`; it prints NOTRUN without them. Cleanup: `fusermount3 -uz` any
leftover `efs-k38-*` mount, then `edelete --force --delete` the kept
directory.

## Remaining

- **NOTRUN (parent):** ior hard on orcd-mini (RDMA, 8 ranks, node9901 and
  node9900), repeated as in the deploy record, with `EFS_DCACHE_TRACE=1` on
  one run; pass = no `EIO`, exact data, no `report-stuck`/`report-span-lost`.
  The ior hard throughput drop (47.9 → 16–21 MiB/s) is not addressed here.
- **Finding (new, outside K38): two writing clients on shared chunks.**
  `--clients 2` (the writers split over both mounts) fails in about half the
  runs on the base and on this branch alike: REPORT units are answered BUSY
  for the whole 8 s budget (`report-receipt … phase=unit rc=-13`), the
  outcome stays unknown, `EIO`. The servers show unit transactions
  coordinated by one node left prepared until `txn-recover … -> ABORT` 5–20 s
  later while the other client's REPORTs on the same shards wait. Not part of
  the gate until fixed; traces in `~/efs-artifacts/k38-20261025/two-client-busy/`.

## Raw evidence

intel2 `~/efs-artifacts/k38-20261025/` (test outputs, the loopback series
summaries and the logs of every failed run, the install-order and
classifier traces, the gate output); `SHA256SUMS` there, hash
`b8d83c80…8cb68` (sha256 of `SHA256SUMS`, 111 files, 5.9 MB).

## October 27 continuation

The later xorinox full pools reproduced shared-chunk application errors on
unchanged devel and Task G. Historical scoped results above do not close
that counterexample. [Task G repair and retained evidence](k38-task-g-repair-20261027.md)
records the new complete failure capture, reviewed repairs, original-code
negative controls and pending full-suite requalification. No recurrence of
the original missing-ABORT defect is inferred from the test name alone.

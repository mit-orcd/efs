# K14 / W77 — atomic multi-chunk publication and observation (I24)

**Purpose.** Close the I24 gap in [W77](../backlog/work-items.md#w77): one
FUSE write request that touches several chunks must become visible all at
once, and a multi-chunk read must return one state of the file, never a
splice of old and new chunks. Alpha 1.0 register row K14 (FIX, D40,
Oct 23).

**Verdict.** PASS for the scoped gates below: unit tests, the full
`make test`, and a private three-daemon TCP cluster on intel2 (steady
rewrites, leader changes, writer-client SIGKILL and remount, undecided,
committed-unresolved and aborted publications). A control run of the same
workload on devel `bf16436e8` (without this change) tears reads.
NOTRUN: RDMA, orcd-mini, multi-host clusters. Fixed on branch `devel-k14`,
not merged into devel.

## Contract

The atomicity unit is one FUSE write request (at most `max_write`, 1 MiB,
so at most 9 chunks of 128 KiB) together with every unpublished write
request of the same client that shares a chunk with it (their bytes sit in
one chunk image, so they can only become visible together). The closure is
capped at `EFS_PUB_UNIT_MAX` = 64 chunks; a write that would grow it past
the cap first publishes the file's pending units.

1. **Publication is all or nothing.** Every chunk mapping of a unit becomes
   visible at one decision (the DECIDE record at the file's inode shard) or
   none does. A client killed after sending its REPORT, a host that dies
   holding the transaction, or a leader change cannot leave a partial unit:
   an undecided transaction is aborted by recovery (5 s), a committed one is
   resolved by recovery or a retry, and a stage whose commit cannot apply
   stays and keeps its chunk refused (fail closed).
2. **Both metadata groups.** Lane shard = inode shard + lane × stride with
   an odd stride, so adjacent chunks always alternate between the odd-shard
   (group 0) and even-shard (group 2) groups: every unit of two or more
   chunks spans both groups and is published as a cross-group transaction.
   It cannot span both groups only when it changes one chunk (a one-chunk
   write, or a unit whose other chunks are already published); that is one
   Raft entry in one group, atomic by itself.
3. **Observation.** A read request covering two or more chunks (at most
   `max_read`, 512 KiB) returns the mappings of one server snapshot of the
   file: a staged chunk reads as its previous mapping while its unit is
   undecided or aborted, and is refused (BUSY, retried by the client) once
   the unit is committed and not yet resolved, or when the decision is
   unknown; a reader that cannot validate returns EBUSY, never a mix.
4. **Idempotent retries.** The transaction id derives from the REPORT's
   client-minted publication id, session, file and first chunk: a re-sent
   REPORT re-PREPAREs the same staged records (OK, host time excluded),
   joins the same decision, and re-RESOLVE is a no-op. A changed request
   under one id is STALE.

Not claimed: atomicity above `max_write`/`max_read` (kernel splitting is the
documented open problem); a reader in the *same* client as the writer, which
reads its own unpublished bytes from the local cache (local read-your-writes,
not publication); the new EOF of an extending write, which can become
visible ahead of a held-back unit's data (sizes ride every record of the
REPORT, unchanged by this work).

## Change

Branch `devel-k14`, commit `00c14c355` on devel `bf16436e8`.

- **Decision record and staged publication** (`include/efs/meta_apply.h`
  "W77/K14", `src/meta/meta_apply.c`, `src/meta/txn.c`, `include/efs/txn.h`,
  `include/efs/kv_key.h`). PREPARE kind `EFS_TXN_PUBLISH` stores a chunk's
  immutable publication under `EFS_KV_KIND_PUB_STAGE` on its lane shard after
  running the ordinary publish checks without writing (`publish_inner_as`,
  dry mode); the stage makes every other writer of that chunk BUSY
  (`meta_write_check_as`). The inode shard is always a participant and the
  coordinator: its PREPARE checks FileID and content epoch and leaves a pin
  that makes truncation and content fences BUSY until RETIRE. RESOLVE COMMIT
  applies each stage through the ordinary publish in the KV batch that
  deletes it (`efs_meta_apply_txn_resolve`), ABORT deletes it, RETIRE removes
  the pin (`efs_meta_apply_txn_retire`), DROP and the recovery pending scan
  include stages. Read validation: `efs_meta_pub_stage_visible`.
- **Server REPORT** (`src/server/raft_host.c`, `src/server/handler.c`,
  `include/efs/protocol.h`). Records carry `EFS_CHUNK_REC_F_UNIT_NEXT`; the
  REPORT carries `EFS_REPORT_F_PUBID` (mint time, sequence). A unit with two
  or more mapping changes runs `host_pub_unit_commit` after the batch: PREPARE
  pin and stages (one cohort), DECIDE, RESOLVE; a STALE member loses the whole
  unit (the client replays it). The forward path carries the publication id.
- **Server GETCHUNKS.** `host_getchunks_collect` answers stages by decision
  and records undecided ones; the window is rechecked, then collected again
  after a fresh ReadIndex on every group it read and must match.
- **Client** (`src/client/write.c`, `efs_fuse.c`, `ops.c`, `read.c`,
  `inode_rpc.c`). Both FUSE write callbacks open and close a unit; REPORT
  shaping carries a unit only whole, when no write holds it and every chunk's
  record names its newest landed object, else holds it back (re-marked); a
  sync REPORT waits for held units. Every GETCHUNKS reply stamps its mappings
  with one snapshot id (`efs_chunk_entry.map_snap`); a multi-chunk read and
  `read_refs` use one snapshot (`efs_client_map_snapshot`, validated across
  windows), recheck after the fetches and retry up to four rounds.
- **Fault gate** (EFS_FAULTS builds only): `EFS_FAULT_PUB_BARRIER` parks a
  unit after PREPARE or after DECIDE, or aborts it after PREPARE.

No compatibility: the new KV kind, PREPARE kind and REPORT flag take effect
on a fresh format; an older peer refuses the unknown flag (INVAL).

## Reproduction

Prerequisites: Linux host with FUSE 3, two build trees of the tested commit
(normal and `EFS_FAULTS=1` for `efsd`), a writable directory, free ports
PORT+1..PORT+3.

```bash
make -j8 && TMPDIR=<disk-backed dir> make test
```

```bash
make -j8 EFS_FAULTS=1 efsd
```

```bash
scripts/k14_atomic_cluster.sh run <build-tree> <empty-dir> 18800 <faults-tree>
```

```bash
scripts/k14_atomic_cluster.sh stop <build-tree> <empty-dir> 18800 <faults-tree>
```

Then delete `<empty-dir>`. Control without this change: build devel
`bf16436e8` and run `K14_PHASES=1 scripts/k14_atomic_cluster.sh run
<devel-tree> <empty-dir> 18800` from this branch (phase 1 needs no fault
hooks). The script prints one PASS/FAIL line per check; raw logs stay in
`<empty-dir>/logs`.

Workload (`tests/live/k14_atomic_publication.c`): two files rewritten by one
writer each on mount A as one 1 MiB `O_DIRECT` `pwrite` + `fsync` per
version — `aligned.dat` [0, 1 MiB) (chunks 0–7) and `straddle.dat`
[64 KiB, 1 MiB + 64 KiB) (chunks 0–8, partial head and tail). Every 8-byte
word encodes (version, offset). Three readers per file on mount B (another
client) issue 512 KiB `O_DIRECT` reads at random 4 KiB-aligned offsets
inside the region, reopening every 64 reads; a read is torn unless every
word matches the version of its first word.

## Tested source and environment

- Source: `devel-k14` commit `00c14c355` (code and tests; this page is the
  following docs commit). The intel2 trees were `git archive` copies built
  with `EFS_GIT_ID=00c14c355c3f`; every binary reports
  `00c14c355c3f (branch devel-k14)`. sha256: `efsd`
  `c8fe1dd5652b3c23d098d394a8f503a217d891f760026e5963bcbbcae7b475dd`,
  `efs-fuse` `88ce8e7659d10efcde25736c80faf9232c0ecad4fbc8c4b46169cf4622bf78d2`,
  `efs-mgmt` `df0a1972364f761481fe0e82148b09b35fa86ef2d5ec93b84f4ee8643f395da2`,
  fault-build `efsd` (`EFS_FAULTS=1`, used by the cluster)
  `9e3e590645d392ead33064e4b7e61bc05d28bec7d4adb38bcadbf62333293ee7`.
- Control: devel `bf16436e8` (`git archive`, build id `unknown`).
- Host: intel2 (Rocky 10, 8 CPU, 15 GB), shared with the user's live EFS
  cluster, which was not touched. Work under `$HOME/efs-k14`, `TMPDIR` on
  the home disk.
- Cluster: three `efsd` on 127.0.0.1 ports 18801–18803, TCP only,
  `EFS_MD_RAFT_N=3`, buffered I/O, quota 4G each, fresh format,
  `EFS_RAFT_OBS=1`; two `efs-fuse` mounts (writer client A, reader client B)
  under the work directory; default client settings
  (`EFS_READ_MAP_TTL_MS` 200).

## Results

| Gate | Result |
| --- | --- |
| `make -j8` | PASS (the existing `reclaim.c` maybe-uninitialized warning only) |
| `TMPDIR=$HOME/efs-k14/tmp make test` | PASS: exit 0, no `^make(\[[0-9]+\])?: .*Error` line, 3 min 51 s; 67 unit binaries and 335 Python tests run (`test_make_test_recipe`); NOTRUN only RDMA, valgrind, fault-build and cluster-script items |
| `tests/test_pub_stage` (new) | PASS: stages invisible, exclusive (ordinary writer and other txn BUSY), idempotent (host time excluded, changed request STALE); pin BUSY for epoch fence and shrink until RETIRE; undecided reads previous + recheck, committed-unresolved BUSY, aborted previous, reclaimed decision BUSY, unreachable coordinator an error; COMMIT/re-RESOLVE, ABORT, refused PREPARE, DROP, recovery pending scan |
| `tests/test_pub_units.py` (new) | PASS: merge, cap (AGAIN), held back on writer / PUT in flight / newer object / missing record, whole-unit framing, retirement only of the unchanged carried unit |
| Changed source-slice tests | PASS (`test_report_forward`, `test_report_frame_boundary`, `test_wb_fault`, `test_read_refs_window`, `test_read_failure_admission`, `test_report_pressure`, `test_report_snapshot_locks`, `test_txn_apply_exact`, `test_txn_decision_sweep`) |
| Cluster gate `k14_atomic_cluster.sh` | **PASS, 22/22 checks** (`RESULT fails=0`), table below |
| Control on devel `bf16436e8`, phase 1 | **FAIL as expected: 112,018 of 709,304 reads torn** (15.8 %); the first five logged per reader all split at a 128 KiB chunk boundary (for example version 1003, then 1004 from offset 524288) |

Cluster gate, one run (per phase: reads, torn reads, version changes seen
by the readers):

| Phase | Reads | Torn | Observation |
| --- | --- | --- | --- |
| 1 steady, 40 s | 65,364 | 0 | 3,018 version changes |
| 2 leader changes, 40 s | 15,460 | 0 | 24 accepted step-downs (both groups), 1,578 version changes |
| 3 writer client SIGKILL + remount | 215,512 | 0 | killed mid-write (42 writer errors: ENOTCONN/ECONNABORTED), writers resumed and published after the remount |
| 4 undecided (parked after PREPARE), leader change while parked | 9,639 | 0 | every read was the last published version 5578; after release the file is the new version |
| 5 committed, unresolved (parked after DECIDE) | 3 | 0 | readers were refused and retried until recovery resolved the commit (`txn-recover … age=5.9s -> COMMIT (resolved)`), then read the committed version 5580 only |
| 6 aborted after PREPARE | 23,552 | 0 | 26 units aborted; nothing past version 5580 visible; the forced-abort fsync failed EIO; after release writers publish again |
| final | — | — | both files one version, equal to the last published (aligned 6619, straddle 5468) |

Server counters (`raft-obs-w77`, summed over the three nodes): 3,093 units
committed through the transaction for about 3,096 published versions, 45
unit refusals (leader changes and the injected aborts), 26,209 GETCHUNKS
collects BUSY on a committed-unresolved stage, 0 double collects that moved.
The double collect never fired in this workload; the stages and the client
snapshot rule carried every case it exercised.

Kept captures (outside Git) on intel2 in `$HOME/efs-artifacts/k14-20261023/`:

| File | sha256 |
| --- | --- |
| `make-test.log` | `fdd871afdd7a07d0037983d4e8dc7be107c4dff36e5df30e5b499bfe40fc4529` |
| `gate.out` | `077ea011e97567d8321f983b251200329fbbec4b2937c37f0b2ab07eb50b5fba` |
| `gate-logs.tar.gz` (daemon, client, reader and writer logs) | `19e6fd45a6e1e4bca24ed62f41787fb6d15883f20a1069aa7d85027a5751ecf5` |
| `control.out` | `378555b29434eb044884e56562281c509da1c95fd1499cbbbe6a4a25553a64f5` |
| `control-logs.tar.gz` | `7e243e27d79cc1adc46c55a4ec567a038b9fb12092fd00062e2b24dc8346d0a1` |

## Limitations and remaining gates

- **NOTRUN:** RDMA transport, orcd-mini, multi-host clusters, direct-I/O
  servers, hardware power loss.
- **Same-client reader.** A reader in the writer's own client can still see
  a multi-chunk write half-copied into its local cache; local cache
  atomicity is not addressed here.
- **Extending writes.** A REPORT's size record raises the EOF on every
  carried record, so a new EOF can be visible before a held-back unit's
  data (pre-existing; only overwrites inside the size were gated).
- **Cost, not measured.** A multi-chunk publication is one PREPARE per
  chunk plus the pin, a DECIDE and one RESOLVE per shard instead of one
  REPORT entry per group; a multi-chunk GETCHUNKS window takes a second
  ReadIndex round; a multi-chunk read reuses cached mappings only when they
  come from one snapshot. The gate's readers completed about a tenth of the
  control's reads in the same 40 s under constant rewriting; this is a side
  observation of a correctness run, not a performance measurement.
- **Committed-unresolved readers wait.** They are refused until RESOLVE
  (normally one round; up to the 5 s recovery age when the coordinating host
  is stuck, as in phase 5).
- **Truncation waits for RETIRE.** The pin keeps shrinking truncate and
  content fences BUSY until the decision is retired (the finisher right
  after RESOLVE; the sweeper after 5 s if that is lost).
- **Clock.** The transaction time is the client's mint time; more than 30 s
  of skew falls back to host time, and a retry is then a new transaction
  (still exclusive through the stages).
- **Unit cap.** A closure past 64 chunks publishes the file first; writes
  above `max_write` (non-FUSE callers) claim nothing.
- The session-fence interplay with an already committed stage (the fence
  does not wait for its RESOLVE) is not separately gated.

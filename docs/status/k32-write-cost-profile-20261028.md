# K32 write cost profile — October 28, 2026

Task U complete in scope: `agent/devel-task-u`, base `b2d57e421`, tested
`75db7afb0`: **524/0 FAIL/11 NOTRUN, INCOMPLETE**, 824 s. Raw: xorinox
`/home/maik/efs-artifacts/taskU-20261028/`. Twelve 1 GiB rows PASS byte-pattern
checks; no product source changed. **IOR NOTRUN; D4 baseline failure OPEN.**
Benchmark and earlier gate failures remain recorded. This does not close
K32 or D4; the readback/coherence limits below apply.

## Scope and method

Xorinox, Ryzen 9 7940HS (8 cores / 16 threads), Rocky/RHEL 10, kernel
`6.12.0-211.67.1.el10_2.x86_64`, `/home` on XFS/LVM, FUSE 3.16.2.
The fixture disk is the KINGSTON OM8PGP41024Q-A0 NVMe, not `/data1`'s HP SSD.
One fresh private loopback three-member TCP cluster per case, O_DIRECT,
8 GiB quota per daemon. Both metadata groups have all three voters. Existing
`EFS_BATCH_OBS`, `EFS_RAFT_OBS` and `EFS_PUT_TICKET_DBG` diagnostics are enabled;
product behavior is unchanged. No other host or existing cluster is used.

Each primary case writes 1 GiB sequentially into **one file per writer**,
with one directory per writer. Requested I/O is 1, 4 or 16 MiB, with one or
eight writers (1 GiB or 128 MiB per file respectively). Each file's close
confirms publication (or the direct
API's explicit synchronous per-inode REPORT); background REPORTs can publish
earlier. A checked cluster `data-commit` and
quiescent applied frontier finish the measurement. Every file is then read
through the mounted client: exact file count, EOF, aggregate bytes and every
byte equal to `A`. In direct mode this is a separate FUSE reader process;
in FUSE mode it is the **same writer mount and may use its client caches**.
Thus all bytes are checked at the filesystem API, but independent fresh-mount
verification of the FUSE cases is NOTRUN. The repeated `A` pattern also
cannot detect substitution by another same-sized file or version. These
measurement checks are not full storage, coherence or K14 atomicity
qualification.

The direct executable is a **measurement-only library client**, explicitly
authorized by the user after the benchmark failure below. It uses normal
node discovery, committed membership, session setup, dcache initialization
and `efs_client_enable_meta_batch(4096)`, then checked create, synchronous
`efs_client_write`, REPORT and shutdown. It does not run the FUSE dcache
write-acceptance pipeline: this is a C API comparison, not an interchangeable
implementation of K14's FUSE publication unit. Direct startup/session teardown
are inside its measured process; the mounted client's startup/teardown are
outside the FUSE window. Fixed session overhead matters most for small pilots.

Primary direct source is `ed580cc83`, aligned FUSE source `b081e9570`.
The direct C workload is unchanged between these heads; only the
FUSE measurement buffer and its fast test changed. Every individual cluster
uses matching binaries. There is no mixed-build cluster or product A/B.

Requested syscall I/O is also distinct from a FUSE callback: the kernel may
split a 4/16 MiB `write` into multiple callbacks (the client bounds `max_write`;
kernel page limits can reduce it further). These rows cannot prove that a
16 MiB syscall creates one publication unit. The primary workload follows IOR's sequential-file shape, but this loopback
traced run is not its native-speed real-host throughput gate.

Accounting checks:

- Before/after `raft-status` captures from all three members agree on commit
  and applied frontiers for both groups. Three stable samples bound the window.
  The valid CRC-checked RFL1 ENTRY records reconcile exactly with applied deltas
  per group and agree across replicas. Applied ENTRY bytes are distinguished
  from physical valid bytes appended between WAL samples: background work can
  append after a status sample or apply an entry appended before it. Boundary
  counts are retained; neither case is silently dropped from its own metric.
  A reserved zero tail is excluded; a
  rotation, CRC error or unexplained index fails the measurement.
- WAL bytes count valid appended records, not allocated file size. The main
  table uses one replica's two-group sum. Applied ENTRY bytes have three
  copies when the decoded streams agree; physical boundary appends can differ
  by replica. Nested commands are separately counted and
  never mistaken for additional Raft entries.
- Ticket admissions count unique identities in ADMIT/ADMIT_BATCH records.
  Settlement counts successful terminal retirement counts/masks from the
  measured client's existing diagnostic, not merely SETTLE proposals. The
  diagnostic is snapshotted before FUSE unmount.
- REPORTs count successful framed sends and fully received replies, including
  partial reads and resumed syscalls, from per-thread `strace -ff` files.
  Failed EAGAIN sends do not count. Nonzero equal send/reply totals are required.
- `strace -f -c` counts daemon fsync/fdatasync syscalls across all threads.
  These are syscall counts, not physical flushes, and include metadata/data
  work, final COMMIT and background work; they do not identify each barrier's
  causal command. CPU comes from `/proc/PID/stat` deltas and GNU time inside
  the client tracer, excluding tracer CPU and post-window verification.

Tracing changes scheduling, cohort formation and barrier frequency. Counts
and CPU describe the instrumented runs; they cannot establish native IOR
speed, a CPU bottleneck, or the causal fraction of the historical regression.

## Primary matrix results

The six `ed580cc83` direct and six `b081e9570` aligned FUSE cases PASS their
byte/EOF/count checks: **12 GiB written and every byte read back**. Each row
is one run, not a distribution. Leaders
are the raw zero-based Raft voter indices reported for groups 0/2; term 1
and the leader pair remained unchanged within every row. Elections chose
different pairs between rows, so cross-row timing is not a controlled
leadership A/B. Ticket counts are admitted identities, not logical chunks.

| API | I/O MiB | writers | window s | leaders g0/g2 | entries/MiB g0 / g2 | valid WAL KiB/MiB | tickets admitted / retired per MiB | REPORT/MiB (total) |
| --- | ---: | ---: | ---: | --- | --- | ---: | --- | --- |
| direct | 1 | 1 | 46.48 | 0/0 | 4.370 / 4.243 | 5.373 | 8.000 / 8.000 | 0.0029 (3) |
| direct | 1 | 8 | 31.18 | 0/1 | 5.020 / 4.637 | 5.435 | 8.000 / 8.000 | 0.0176 (18) |
| direct | 4 | 1 | 46.30 | 0/0 | 4.284 / 4.250 | 5.368 | 8.000 / 8.000 | 0.0029 (3) |
| direct | 4 | 8 | 27.09 | 0/0 | 5.022 / 4.637 | 5.436 | 8.000 / 8.000 | 0.0166 (17) |
| direct | 16 | 1 | 165.95 | 0/1 | 4.291 / 4.354 | 5.374 | 8.000 / 8.000 | 0.0029 (3) |
| direct | 16 | 8 | 31.28 | 0/2 | 5.021 / 4.638 | 5.436 | 8.000 / 8.000 | 0.0186 (19) |
| fuse | 1 | 1 | 21.06 | 0/2 | 4.198 / 2.850 | 7.866 | 8.000 / 8.000 | 0.0791 (81) |
| fuse | 1 | 8 | 19.45 | 0/0 | 3.319 / 3.447 | 7.829 | 8.000 / 8.000 | 0.1055 (108) |
| fuse | 4 | 1 | 18.97 | 0/0 | 3.392 / 3.209 | 7.790 | 8.000 / 8.000 | 0.1543 (158) |
| fuse | 4 | 8 | 21.30 | 0/1 | 4.665 / 1.817 | 7.828 | 8.000 / 8.000 | 0.1025 (105) |
| fuse | 16 | 1 | 21.31 | 0/2 | 2.822 / 4.169 | 7.810 | 8.000 / 8.000 | 0.0625 (64) |
| fuse | 16 | 8 | 22.15 | 0/2 | 4.559 / 1.696 | 7.803 | 8.000 / 8.000 | 0.1143 (117) |

Valid WAL bytes above sum both groups on replica n1 and exclude reservation.
The aligned 4 MiB/single-writer row includes one 42-byte applied entry that
was already appended before the first WAL sample: applied ENTRY bytes are
8,168,058, physical newly appended bytes 8,168,016. This exercises the
corrected boundary accounting. The other primary n1 totals agree; no late
out-of-range append is included as an applied entry. All three replicas
reconcile every applied index. Raw per-node totals and both metrics remain
in JSON. REPORT sends equal complete replies in every row. Every primary
row admits and terminally retires exactly 8,192 identities (8/MiB).

| API | I/O MiB | writers | fdatasync/MiB n1 / n2 / n3 | fsync/MiB n1 / n2 / n3 | CPU s/GiB n1 / n2 / n3 | library client CPU s/GiB | writer CPU s/GiB |
| --- | ---: | ---: | --- | ---: | --- | ---: | ---: |
| direct | 1 | 1 | 17.218 / 15.369 / 15.492 | 0.0049 / 0.0049 / 0.0049 | 22.58 / 11.15 / 11.09 | 2.48 | 2.48 |
| direct | 1 | 8 | 10.248 / 10.033 / 9.576 | 0.0049 / 0.0049 / 0.0049 | 13.68 / 11.53 / 8.51 | 3.03 | 3.03 |
| direct | 4 | 1 | 17.121 / 15.198 / 15.329 | 0.0049 / 0.0049 / 0.0049 | 22.63 / 11.18 / 11.10 | 2.37 | 2.37 |
| direct | 4 | 8 | 13.027 / 8.354 / 9.370 | 0.0049 / 0.0049 / 0.0049 | 18.61 / 8.43 / 8.68 | 2.41 | 2.41 |
| direct | 16 | 1 | 17.202 / 16.838 / 15.671 | 0.1494 / 0.1494 / 0.1494 | 22.16 / 26.73 / 16.47 | 3.17 | 3.17 |
| direct | 16 | 8 | 10.250 / 9.532 / 10.052 | 0.0049 / 0.0049 / 0.0049 | 13.76 / 8.51 / 11.55 | 2.84 | 2.84 |
| fuse | 1 | 1 | 7.573 / 6.989 / 7.118 | 0.0000 / 0.0000 / 0.0000 | 11.09 / 6.97 / 8.96 | 3.66 | 0.05 |
| fuse | 1 | 8 | 7.859 / 5.717 / 5.321 | 0.0000 / 0.0000 / 0.0000 | 14.05 / 6.85 / 6.92 | 4.42 | 0.05 |
| fuse | 4 | 1 | 8.114 / 5.246 / 5.617 | 0.0000 / 0.0000 / 0.0000 | 13.90 / 6.82 / 6.81 | 3.51 | 0.06 |
| fuse | 4 | 8 | 7.468 / 6.938 / 6.879 | 0.0000 / 0.0000 / 0.0000 | 11.53 / 8.89 / 7.01 | 5.23 | 0.05 |
| fuse | 16 | 1 | 7.099 / 6.872 / 7.468 | 0.0000 / 0.0000 / 0.0000 | 8.92 / 6.90 / 10.95 | 3.77 | 0.07 |
| fuse | 16 | 8 | 7.255 / 6.674 / 6.742 | 0.0000 / 0.0000 / 0.2588 | 11.80 / 7.11 / 9.10 | 5.55 | 0.07 |

For direct mode the library client and writer are the same process: those
last two columns **must not be added**. For FUSE they are distinct processes;
the Python writer CPU is additional to the mounted client. Daemon CPU is
process-wide across its threads. Values exclude the tracer processes and
post-window reads. Individual fsync/fdatasync totals, not just their sum,
are retained. These counts are not independent costs to add to Raft entries.

The 165.95 s direct sample is retained unchanged. Artifact timestamps place
about 165.27 s in the direct workload, 0.04 s in final COMMIT and 0.63 s in
the final frontier wait. Its client reports pressure at 7,937 records with
oldest ages 82.7/76.4 s, versus 8,177 records and 22.0/21.6 s in the 1 MiB
single-writer row. Both report zero pressure timeouts. Its leaders are split
(0/1), while the 1/4 MiB single-writer rows co-locate (0/0). It also records
153 fsync calls per node versus 5 in the other direct rows. These are
correlations, not an isolated cause; neither larger I/O nor entry count alone
explains the delay. No slow observation was replaced by a faster repeat.

## Ranked entry costs

Ranked by aggregate top-level entries divided by the **six GiB of FUSE
input**, with the range over its six equally sized runs. Nested PREPAREs and
RESOLVEs are work inside a TXN_BATCH, not additional consensus entries.

| rank | family / top-level tags | entries/MiB mean | range |
| --- | --- | ---: | --- |
| 1 | ticket admission + settlement, 39/42/90/91 | 5.657 | 4.710–6.598 |
| 2 | session admission/lifecycle, 9 | 0.484 | 0.102–1.054 |
| 3 | transaction/publication machinery, 5/6/7/41/83/88 | 0.356 | 0.264–0.482 |
| 4 | initial lane activation/bootstrap, 19/79 | 0.147 | 0.032–0.263 |
| 5 | background packing-index maintenance, 78 | 0.039 | 0.035–0.043 |

Transaction counts include the small initial namespace transactions as well
as publication. Per-row ranks differ: sessions exceed publication in some
eight-writer cases. Direct ticket traffic is 8.252–8.298 entries/MiB, because
the synchronous C chunk loop does not supply the FUSE admission cohort.
FUSE still logs 3,917–6,006 single ADMIT entries/GiB. The first measured
follow-up is admission cohort formation/single fallbacks, with tracing
removed as a scheduling confounder, **not another unmeasured publication
rewrite**. The 1 MiB FUSE single-writer row carries 8,226 PREPAREs inside
196 TXN_BATCH entries, plus two standalone PREPAREs; publication batching
is already effective.

The three entry families are an entry-count ranking, not a CPU or wall-time
attribution. An untraced, repeated run with a recorded stable leadership
layout and IOR-shaped files is needed before changing batching policy. No
extra product switch, longer product deadline or weaker durability is
proposed. The real-host IOR and faulted K13/K14 gates remain separate.


## Gate verification and test-only corrections

| source | commands | FAIL | NOTRUN | seconds | verdict |
| --- | ---: | ---: | ---: | ---: | --- |
| `6f4a05704` | 523 | 0 | 11 | 847 | INCOMPLETE, 12 over budget |
| `ed580cc83` | 523 | 0 | 11 | 806 | INCOMPLETE, 13 over budget |
| `b081e9570` | 523 | 2 | 11 | 807 | FAIL, 11 over budget |
| untouched `b2d57e421` | 522 | 1 | 11 | 900 | FAIL, 14 over budget |
| `75db7afb0` | 524 | 0 | 11 | 824 | INCOMPLETE, 11 over budget |

The two `b081e9570` failures are retained, not overwritten by later results:

- **D4 plateau:** `put_errors=1`, no packer read error, drained backlog zero.
  The test's unconditional finally cleanup removed its detailed fixture;
  original failure stdout remains, so that particular low-level cause is
  unproven. Untouched `b2d57e421`, same host and eight-job full gate, reproduces
  the same assertion with `put_errors=2`. Its retained logs show two segments
  marked degraded after node 3's `NOT_PRIMARY` responses exhausted the existing
  roughly two-second segment PUT retry bound. This is a pre-existing packer
  gate failure, not evidence of NVMe failure. No packer change, longer retry
  window, skipped assertion or relaxed workload is included. That issue stays
  OPEN even if a later run passes. Baseline `EFS_D4_KEEP=1` changes retention
  only; `75db7afb0` makes failed-fixture retention the test's default.
- **W130 directory fault:** one operator `raft-resync` returns BUSY (`-13`).
  The full untouched baseline passes this test once; that is not a claim of
  deterministic reproduction. Source inspection identifies the pre-effect
  receive-persistence guard in `efs_raft_request_resync`: BUSY returns before
  setting `resync_requested` or changing a read proof. A deterministic test
  executes that production function and verifies no state change on BUSY.
  `75db7afb0` corrects only the test's operator admission handling, retrying
  that specific refusal within the **same** recovery deadline. Other errors,
  malformed replies and transport failures still fail immediately. The
  snapshot wait shares the remaining bound (60 scaled seconds for W165;
  300 seconds for W130), rather than getting a new window after admission.
  CREATE, LOOKUP, GETATTR, directory completeness and payload checks keep
  their original no-application-retry rules. Focused W130 cluster run PASS.

[Resync/retention fast test](../../tests/test_resync_admission.py) checks the
real guard, bounded helper, permanent-error refusal and the actual D4 finally
block. [Profile fast test](../../tests/test_write_cost_profile.py) checks WAL
CRC/index windows, batching, ticket identities, partial replies and aligned
sequential workload shape. `git diff b2d57e421 HEAD -- src include` is empty:
all changes are measurement scripts, tests, test wiring and documentation.

The 11 NOTRUN commands/subcases are nine unavailable RDMA-device paths,
missing `~/git/cluster` stop/remount scripts, and the normal-build
`test_follower_bound` fault-only subcase. The latter is a test-wiring gap,
not a hardware absence. No skip is counted as full coverage. Budgets and
product deadlines are unchanged; retained summaries list every overrun.

## Benchmark limitation and retained failures

The unmodified `efs-bench --store` refuses remote payload writes because they
require ticket admission. Its help still advertises that path. The available
`--meta --size` path returns success for payload creation but never enables
metadata batching/dirty tracking. `efs_client_mark_chunk_dirty` and
`efs_client_mark_ino_dirty` return immediately while `meta_batch` is zero;
the final unchecked REPORT has nothing to publish.

The committed-source negative run created sixteen 1 MiB files, sent real
PUTs and exited zero, but verification found `f0` at EOF **0**, with no REPORT
exchanges. This is a failed benchmark baseline, not a passing write test.
No product benchmark fix is included in this measurement task.

Earlier retained pilots include a harness FAIL from using the wrong WAL path
(`mdraft/raft.log` rather than `mdraft/log/raft.log`), the initial zero-REPORT
benchmark FAIL, and successful 16 MiB FUSE and direct API pilots. Failed
outputs are never overwritten or folded into the accepted matrix.

The original fresh-file-per-request 4 MiB/eight-writer measurement also FAILS: its workload
exited zero, but the harness treated a background append after the status
sample as an out-of-window error. This is not evidence of payload corruption;
that failed sample did not reach full verification. `241e035b3` fixes accounting
by selecting every applied index exactly once and reporting physical append
bytes independently. Deterministic tests cover both boundary races. The first
three completed fresh-file cases remain diagnostics only. That original
layout exaggerated lane setup relative to the intended sequential workload;
`ed580cc83` corrects the primary matrix to one file per writer, with a fast
regression test for file count, EOF and payload. All twelve primary cases run
in a new evidence directory; old outcomes are not reclassified. No timeout or durability rule changed.

Raw review then found a second confounder in the first sequential FUSE
matrix: the Python `bytes` payload starts 48 bytes into a page on this host.
The trace contains 1,024 requests of 1 MiB minus 48 bytes plus 1,024 requests
of 48 bytes in its 1 MiB case. Its 4/16 MiB syscalls similarly have short
first requests and 48-byte tails. The installed Linux headers confirm a
40-byte FUSE request header plus 40-byte WRITE header; decoding the returned
request length minus 80 accounts for every payload byte. A pointer probe
confirms offsets 48 for all three `bytes` sizes and zero for anonymous mmap.

Those writes are valid application workloads and passed byte checks; this
is a measurement alignment confounder, **not a product corruption finding**.
The unaligned 16 MiB rows admitted/retired 8,320 object identities for 8,192
logical chunks. `b081e9570` uses a page-aligned mmap buffer and tests alignment
at every write in the fast workload regression. Its six FUSE rows replace
the unaligned rows in the primary table. The direct C workload is unchanged
and does not pass its application buffer through the FUSE page limiter.
All original unaligned outputs, decoded counts and passing byte verdicts
remain in `sequential-ed580cc83/`; they are not erased or called failed
payload checks. Their different elections/scheduling prevent treating the
two runs' timing difference as an isolated alignment speedup.

## Code paths and constraints on follow-up batching

The ranking above uses measured entry-family counts, not an inferred
percentage of wall time. The source identifies these distinct kinds of work:

- **Ticket lifecycle:** `src/client/put_ticket_owner.c` admission queue,
  `admit_cohort` handling and ordered settle pass, RPC framing in
  `src/client/inode_rpc.c`, and ADMIT/SETTLE apply in `raft_host.c`. A batch
  must preserve immutable FileID/object/session identity, admission before
  the first PUT, ambiguous admission retention, per-stream settle order and
  per-ticket reply masks. It cannot retire a still-in-use object or turn a
  lost reply into a new identity. Gates:
  [owner ledger](../../tests/test_put_ticket_owner.py),
  [RPC](../../tests/test_put_ticket_rpc.py),
  [real apply batch](../../tests/test_put_ticket_batch.py),
  [persistent ticket state](../../tests/test_put_ticket.c), and the real-host
  [K13 cluster gate](../../tests/live/k13_ticket_cluster.py).
- **Publication (rank 3):** REPORT shaping and unit tracking in `src/client/write.c`,
  `host_pub_unit_txn`, `host_pub_unit_commit` and `host_pub_units_publish` in
  `src/server/raft_host.c`. Batching a superset must preserve each whole
  multi-chunk unit, newest object identity, epoch and FileID checks, atomic
  visibility, receipt identity across retries and safe handling of partial
  per-item verdicts. Admission/publication/reclamation ordering remains
  unchanged. Gates: [unit tracker](../../tests/test_pub_units.py),
  [entry-count comparison](../../tests/test_k32_entry_count.py),
  [batch apply](../../tests/test_txn_batch_apply.py),
  [publication persistence](../../tests/test_publication_persistence.py),
  and the version-discriminating, independent-reader
  [K14 cluster gate](../../scripts/k14_atomic_cluster.sh), including its
  eight-units-per-transaction phase. This constant-payload matrix does not
  replace that gate or assert it ran here.
- **Session admission/lifecycle (rank 2):** `apply_session_cmd`,
  `host_session_establish_admit_covered` and `host_report_session_admit` in
  `src/server/raft_host.c`, backed by `src/meta/session.c`. Reducing entries
  must retain UUID/epoch identity, durable touched-shard registration before
  establishment/mutation, rejection of fenced sessions and ordered fence
  completion. REGISTER_ESTABLISH is a same-group combination, not permission
  to write another group's rows. Direct's measured shutdown also includes
  its revoke/fence commands, so use the FUSE rows for this ranking.
  Gates: [admission](../../tests/test_session_admission.py),
  [REPORT first touch](../../tests/test_report_session_admission.py),
  [combined-group restriction](../../tests/test_session_combined_group.py),
  [registration verdict](../../tests/test_session_register_verdict.py) and
  [fence finish](../../tests/test_session_fence_finish.py).
- **Fresh-file lane setup:** ACTIVATE_LANE and BOOTSTRAP_LANE, reached from
  client writer-view acquisition through `server_raft_host_lane_bootstrap`.
  It is amortized over a file's first chunks, not a recurring cost of every
  write to an already populated file. Batching must preserve exact FileID,
  content epoch and geometry, cold-lane initialization, forwarding/error
  propagation and replay of the initialized state. Gates:
  [bootstrap RPC](../../tests/test_lane_bootstrap_rpc.py),
  [host](../../tests/test_lane_bootstrap_host.py),
  [export path](../../tests/test_lane_bootstrap_export.py), and
  [recovery](../../tests/test_lane_bootstrap_recovery.c).

Barriers are a separate persistence layer, not additional command entries:
`src/raft/raft_log.c::log_sync_locked` and
`src/kv/kv_wal.c::kv_wal_fsync_to_locked` already share durable prefixes.
Any further coalescing must retain durable-prefix watermarks, persistence
before dependent ACKs/replies, failure propagation and bounded progress.
[Raft store](../../tests/test_raft_store.c),
[sync/rotation fencing](../../tests/test_raft_sync_rotation.py), and
[KV WAL faults](../../tests/test_kv_wal_faults.py) pin those rules. The aggregate
syscall counts alone cannot attribute a barrier to one command family or
justify increasing linger windows, weakening fsync, or adding a product flag.

## Reproduction and evidence

Use a Linux checkout of `75db7afb0` on an otherwise idle host with gcc, Python,
libibverbs headers/library, libfuse3 development files, `/dev/fuse`, GNU time,
strace attach permission, screen, and enough free space for private stores.
Build the normal product, then run:

```sh
export EFS_GIT_ID=75db7afb0 EFS_GIT_BRANCH=agent/devel-task-u EFS_GIT_DESCRIBE=75db7afb0
make -j8 all
python3 scripts/write_cost_profile.py \
  --work "$HOME/efs-taskU-reproduce/work" \
  --output "$HOME/efs-artifacts/taskU-reproduce/matrix"
```

Both directories must be new; the default is all twelve 1 GiB cases. The
driver compiles only `scripts/write_cost_client.c` against `libefs.a` into
the private work directory. `--modes bench --total-mib 16 --io-mib 1
--writers 1` reproduces the original diagnostic in separate new directories
and is expected to FAIL. Do not substitute it for the direct rows.

The final gate command is `EFS_TEST_TARGET=test-all EFS_TEST_JOBS=8
scripts/make_test_summary.sh`; `W130_EVIDENCE_DIR` names a stable per-run log
destination, without changing its workload or verdicts. Long commands run
in screen. Exact launch scripts and environment/build records are retained
with the captures.

Every workload has a 900-second outer measurement deadline; product RPC/close
deadlines are unchanged. Owned idle mounts stop via strict `client.sh stop`;
the driver requires mount absence and checked orderly daemon exits. A failed
mount stop preserves the live servers and records `live-fixture.json` for
investigation. No forced detach or store deletion is part of measurement.

Raw evidence is on xorinox at
`/home/maik/efs-artifacts/taskU-20261028/`. Its **2,339 retained files** have a
fully re-read and verified `SHA256SUMS`, itself SHA-256:
`cdb5b2f978f88c6ebf0e8f86820cdecfc4e8ef0719f10686192edeee431837ff`.

Primary source archives: `ed580cc83`
`306a944df5cc6e229fb66e59724c82b13dd27c2f669c7a67ec15119410f0d62f`,
`b081e9570`
`0298c59e0263c135aa1c1b1b01eb2d2a25c80683099004e1c64a52065efc9b26`.
Final test source `75db7afb0`:
`da59edc607c6f1da8ac2cb63ad7c2175fd5f07ff333d85e0ab2884b5516f3897`.
`source-archives-complete.sha256`, `binaries-*.sha256`, build logs and exact
`run-*.sh` scripts retain all identities and commands. Each cluster used the
matching embedded build ID of its named source. Early uncommitted pilot
drivers are diagnostic only, not primary acceptance evidence.

`sequential-ed580cc83/` holds direct and original unaligned FUSE rows;
`aligned-b081e9570/` holds aligned FUSE rows. Both include results JSON,
status frontiers, measured client/daemon logs, CPU records, traces and checked
stop records. `analysis*.json`, `alignment-probe.json`, `fuse-abi.txt` and
the copied collector scripts document the independent raw analysis. All six
aligned captures contain **1,024 × 1 MiB FUSE WRITE payloads**, accounting
for exactly 1 GiB each. No 48-byte tail is present. All primary syscall
error columns are zero (`primary-sync-errors.json`).

Eighty-one valid-WAL-prefix gzip captures were verified against the original
bytes and opened fully. They are kept for **OPEN K32 cost review**, with
source/capture hashes in `wal-captures*.json`. The twelve primary applied
windows also match `(group, index, term, SHA-256(command))` across all three
replicas, independently of aggregate counts (`replica-command-identities.json`).

Failed W130 stores remain in `failed-w130-b081e9570.tar.gz`, SHA-256
`a979ad680948c1bebd0779fef88a50d7d8c2295fb1ce4e16f887f38abb1acc2f`.
The reproduced baseline D4 stores remain in
`failed-base-b2d57e421-case462.tar.gz`, SHA-256
`5e192d8f8f7bc89302dd4834f7390edcd45fcc084147fcc33c537b0fe9053464`.
Both are kept for their **OPEN investigations**, with verified member
manifests. The original D4 event's deleted fixture cannot be reconstructed
from its retained stdout alone; the retained baseline event is separate.

After checking no owned processes or mounts remained, the private scratch
`/home/maik/efs-taskU-20261028/` was removed with
`~/bin/edelete --force --delete`. `PRUNED.txt` and quiescence records document
the removal of generated payloads, source archives, build trees and passing
stores. Logs, results, traces, captures and the failed archives above remain.
Original passing-store restart replay is forfeited; recreate them from the
committed method. This measurement includes no cold-restart, power-loss,
real-network or RDMA qualification. The host is free of Task U runtime work.

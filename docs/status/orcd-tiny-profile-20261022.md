# ORCD tiny-file CPU profiles — October 22, 2026

**Verdict: analysis complete. Findings 1 and 2 fixed on branch
`claude/pack-validate-once` (unit, private-cluster and `make test` gates PASS
on node9900; see "Fixed on branch"); live CPU A/B on orcd-mini NOTRUN.**
Related work: W125 (packing), W126 (KV maintenance), and enabled `.efs`
indexing. This is a review of existing captures, not a new qualification run.
No deployment, workload, process or original capture was changed.

## Evidence and scope

Read on fstor007. Original evidence is on the shared filesystem:
`/home/erbmi1/orcd/scratch/efs/profile-tiny-20261022/`.
Each of `fcstor003`–`fcstor006` contains `oncpu-30s.perf.data`, `threads.txt`,
`top-self.txt`, `flat-self.txt` and `children-selected.txt`. Raw files stay there;
none are committed here. The saved summaries were checked against raw perf
reports. sha256 of the captures (taken 2026-10-22 on node9900, which mounts the
same home):

| Capture | Bytes | sha256 |
| --- | ---: | --- |
| `fcstor003/oncpu-30s.perf.data` | 2887092 | `dd1f1b95b753d312b885257ed3c67991bb8f14269e286a0280360f5895a5f67d` |
| `fcstor004/oncpu-30s.perf.data` | 11051428 | `b75e9ba3ee790332dfa727d053e344adfc0f69d3c9390c4339269c05644c7c2f` |
| `fcstor005/oncpu-30s.perf.data` | 3998968 | `72ed4343e7ba664a7e8a3a2a9f4d90cb8aa11df29a92da7a4d59e916139ace54` |
| `fcstor006/oncpu-30s.perf.data` | 2525840 | `6ecc8e4e4e70762e81d7a4f220a91c0f8e4262b629e5bbc8957d199671c378a7` |

Recompute: `cd ~/orcd/scratch/efs/profile-tiny-20261022 && sha256sum fcstor00{3,4,5,6}/oncpu-30s.perf.data`. Despite its name, `threads.txt` groups by comm and is not a per-TID
utilization measurement.

All four cached ELF binaries embed source ID **`08cc16af4f77`**. The code
analysis below used that Git revision, not the newer local working tree.

| Hosts | ELF build ID | Embedded build timestamp |
| --- | --- | --- |
| fcstor003–005 | `fee2972e319e2d322277ef9cc2460524f3eb073f` | `2026-10-22T04:32:30Z` |
| fcstor006 | `221bace4e060131551cc2569d0aaad9ff1a4142f` | `2026-10-22T04:32:51Z` |

Different ELF IDs therefore do not by themselves establish different source
revisions. Cached ELF files are under
`~/.debug/tmp/efs/efsd/BUILD_ID/elf` on fstor007.

The capture headers label the run October 22, 08:51:42. The header display
records no timezone; the four files' modification times are 08:51:42 −0400
(EDT), matching that label, so the capture ended at about **12:51:42 UTC**
(derived from mtimes on the shared filesystem; the capturing hosts' own
timezone setting was not checked). Each records approximately 30 seconds at 499 Hz, event
`cycles:P`, process-attached with call stacks. All report zero lost samples.
The inspected fcstor004 header reports AMD EPYC 7402P, 24 online CPUs,
263318216 KiB memory and kernel `5.14.0-687.39.1.el9_8.x86_64`.
RDMA processing, packing and `.efs` indexing are observed in the stacks.
Exact writer concurrency, file counts, quotas, direct/buffered data-I/O mode,
expiry/container settings and Raft leadership were not independently established
from this evidence. Do not infer them from earlier deployments.

## Results

Percentages are inclusive sampled CPU cycles **within each daemon**, not
operation latency or percentage of machine capacity. Nested rows must not be
added. No before/after baseline or throughput comparison was captured here.
**These percentages are not comparable with orcd-mini or later runs:** the
workload, its concurrency, data sizes, quotas, I/O mode and leadership were
not established for this capture, so a different share elsewhere is not a
regression or an improvement.

| Inclusive path | fcstor003 | fcstor004 | fcstor005 | fcstor006 |
| --- | ---: | ---: | ---: | ---: |
| `server_write_fragment_with_sum_sync` | 22.35% | 5.16% | 14.52% | 27.08% |
| `compactor_main` | 13.83% | 5.14% | 22.79% | 7.99% |
| `seg_bloom_main` | 5.69% | 2.28% | 9.37% | 3.36% |
| `recv_poller` | 3.94% | 3.86% | 3.16% | 5.11% |

### 1. Repeated whole-segment validation is avoidable packing work

On fcstor004, `efs_pack_ledger_init` accounts for 14.29% inclusive cycles;
`efs_pack_read` 14.28%, and `efs_pack_validate` 14.27%. These overlap:

```text
pk_job_run -> pk_segment -> efs_pack_ledger_init
                        -> efs_pack_read -> efs_pack_validate -> hashing
```

At `08cc16af4f77`, `src/common/pack_ledger.c` validates the segment once, then
calls `efs_pack_read` for every record. `src/common/packed_segment.c` makes each
read validate the entire segment again: header/body checksums, every record's
payload checksum and nested duplicate-inode checks. For N records, ledger
initialization alone performs N+1 full validations plus one more whole-image
hash for the body digest: **N+2 whole-image hashes**. Each validation's
pairwise duplicate-inode check is O(N²), so ledger init does **O(N³)** inode
compares (512 records: ~67 M). This is a confirmed repeated-work mechanism,
not a measured speedup estimate.

Code review against `08cc16af4f77` (pack code unchanged on devel since) found
the same pattern elsewhere:

- the image `pk_segment` hands to `efs_pack_ledger_init` was built by
  `efs_pack_build` moments earlier, so all N+1 validations re-check bytes
  this process just wrote;
- compaction (`pk_compact`) re-read each source segment through
  `pk_read_segment` (one validation), then `efs_pack_record_count` and
  `efs_pack_record_at` per record, each validating again: N+2 validations
  per source segment;
- `pk_read_packed_base` fetched, decoded and validated a whole segment,
  then validated it again in `efs_pack_read`, for **every** file whose base
  is a packed record, even when several such files share one segment.

Recommendation: validate the pinned immutable image once, then validate each
reference against that validated image. Preserve all identity, bounds and
checksum guarantees; do not introduce a generally callable unchecked read or
reuse validation after image mutation. Test corruption and wrong references as
well as CPU cost versus record count.

### 2. Searching for abandoned packing work costs CPU too

On fcstor004, `pk_abandon` is 5.24% and its callback 4.71% inclusive.
`pk_group` calls it on **every pass** (default 1 s), including the early
return taken when fewer than three voters are live, and `pk_abandon` starts
an object-prefix scan from the beginning each time. The callback decodes objects before checking phase, pending records
and ownership. It stops after collecting 64 candidates; with no qualifying
candidates it can inspect the whole prefix. The scan's 128-entry yield interval
is not a bound on total work.

Recommendation: measure scanned versus actionable objects; use a bounded,
resumable reconciliation scan or pending-object index. Preserve eventual cleanup
across leadership changes. The function's presence in the profile does not prove
that objects were abandoned or that requests failed.

### 3. Asynchronous `.efs` work still competes for shared resources

`host_query_thread` is 9.34% of fcstor004 cycles, with
`efs_query_catalog_build_disk_checked` at 9.33% and
`efs_query_disk_build_checked` at 9.19%. Incremental scans, comparisons and
sorting are visible. Asynchronous execution does not remove CPU, memory-bandwidth
or storage competition. These samples do not establish failed refreshes,
repeated full rebuilds or their effect on foreground latency.

### 4. LSM maintenance is substantial

On fcstor005 the separate compaction and Bloom worker paths total about 32% of
sampled cycles. Comparisons, memory copies, CRC and segment I/O appear underneath
these paths. This supports measuring maintenance interference under W126, but
neither mutex blocked time nor exclusive-phase duration is established here.

### 5. Fragment persistence remains material; RDMA is present

Fragment writing is 14–27% on three nodes, including XFS allocation and fsync
paths. CPU samples within fsync are not its wall-clock wait duration. RDMA
receive processing and copy paths are present; these captures do not demonstrate
that every payload uses RDMA, measure wire throughput, or identify transport as
the limiting resource.

## Reproduce the analysis

Requires Linux perf able to read these files and the matching ELF/debug cache.
On fstor007, run the following read-only commands for each host directory:

```sh
cd ~/orcd/scratch/efs/profile-tiny-20261022
perf report -i fcstor004/oncpu-30s.perf.data --header-only
perf buildid-list -i fcstor004/oncpu-30s.perf.data
perf report -i fcstor004/oncpu-30s.perf.data --stdio --children \
  --sort symbol --percent-limit 1 --call-graph none
perf report -i fcstor004/oncpu-30s.perf.data --stdio --no-children \
  --sort symbol --percent-limit 1 --call-graph none
```

The original recording command in fcstor004's header was:

```sh
/usr/bin/perf record -F 499 -g -p 1575053 -o /tmp/efsd-fcstor004.perf -- sleep 30
```

That PID is historical, not an instruction to record it again. A fresh
reproduction needs the current daemon PID and a separately named output, exact
workload parameters, matching binary identity and concurrent throughput/latency
measurements. No cleanup is needed for this read-only analysis; retain captures.

Some kernel addresses remain unresolved when reporting on fstor007; use matching
kernel symbols before making fine-grained kernel attribution claims. The saved
kernel-symbol summaries are supplementary evidence. On-CPU captures cannot
quantify lock waits, disk waits, RPC latency or files/s limits. Follow-up should
pair off-CPU/lock-duration evidence and storage latency with foreground rates.

**Priority:** remove repeated packing validation; bound abandonment scanning;
then measure foreground latency against KV and query maintenance. No durability
relaxation or disabled integrity checks are proposed.

## Fixed on branch `claude/pack-validate-once`

Branch from devel `dd43b4015` (not merged, not pushed):

| Commit | Change |
| --- | --- |
| `207dde6b4` | `struct efs_pack_view`: `efs_pack_view_open` validates once and seals the exact image pointer, geometry and header; `efs_pack_view_read` / `efs_pack_view_record_at` keep every per-reference identity, bounds, generation, epoch and digest check and hash that record's payload. Duplicate-inode check sorted (O(N log N)). Ledger init, compaction enumeration and packed-base reads validate once per image. `efs_pack_record_count/at` removed. |
| `660c6621c` | `pk_read_bases`: packed-base files of a pass are grouped by segment; each segment is fetched and validated once. Counters `base_segments`, `base_files`. |
| `745af27f5` | `efs_pack_abandon_scan`: at most 1024 keys per pass from a per-group cursor (restarted on a term change, wraps at the end), cheap phase/pending/owner check before the full decode, which runs only for candidates and outside the scan. Counters `abandon_scanned`, `abandon_actionable`, `abandon_refused`. |
| `e223b88d5` | W125 private-cluster scripts take the node address from `W125_ADDR` (node9900 is `10.1.99.0`). |

**Unit evidence** (macOS and node9900): `tests/test_pack_validate_once`
asserts exactly one whole-image validation per ledger init (512 records) and
per compaction enumeration, no validation per read through an open view, and
refusal of a payload flip (before and after open), wrong digest, inode/record
generation, epoch, segment generation, offset, length and inode, a duplicate
inode with valid checksums, a forged, zeroed or retargeted view and a header
changed after open. Its abandon part sweeps 41 objects in windows of 8: the
first window finds nothing, each of the 10 abandoned objects beyond it is
found exactly once, a corrupt object is refused, not cancelled.
`tests/test_pack_base_reads.py` asserts one segment read and validation per
segment for ten files over two segments, per-file stale-reference refusal and
failure isolation. Existing pack tests are unchanged in behaviour.

**Micro-benchmark** (`scripts/pack_validate_bench.sh 50bbd693a HEAD`, one
512-record segment, Apple M2, portable BLAKE3): ledger init ~160–198 ms →
~0.5–1.6 ms; compaction-style enumeration ~158–199 ms → ~0.4–0.7 ms.

**node9900 private gates** (2026-10-22 13:52–14:27 UTC). Build: release tree
`22a2207cd8a5` plus `git diff 22a2207cd e223b88d5 -- . ':!docs'` (code
identical to `e223b88d5`; build ID label `dea1f13ba-pvo`, an earlier hash of
the same code), gcc on RHEL 9.8, normal and `EFS_FAULTS=1`. efsd sha256
`a9bbb9bf2674a956…` (normal), `2f9eaab5caa5fdf2…` (faults); baseline efsd
`0e8603b6227e431d…` (release `22a2207cd8a5`). Clusters: 3 efsd on node9900,
RDMA `mlx5_0`, `--direct-io`, quota 200G, one efs-fuse mount, all under
`/tmp`; ports 17681–17683, 17741–17743, 17751–17753; orcd-mini untouched.

| Gate | Command | Result |
| --- | --- | --- |
| `make test` | `scripts/make_test_summary.sh` in a copy of the build tree | 819 commands, 814 PASS; 5 FAIL from the environment only: 4 ASan-linked compiles (`libasan.so.6.0.0` absent on node9900: `test_readindex_pair`, `test_raft_ack_window`, `test_host_outbox`, `test_stage_evict_lru`) and `check-architecture.py` links into `results/` (not in release trees). All pack tests pass. |
| Packer drain A/B/A′ | `w125_pack_drain.sh REL OUT 17680 300000` | all 300,000 packed each run; drain 141 s / 143 s / 144 s (base / branch / base). Summed pass timings: segment phase 17.2 s / **9.3 s** / 17.4 s, whole passes 27.5 s / **20.2 s** / 27.9 s; read phase unchanged (8.6–8.9 s). Wall-clock drain is bounded by the write-then-restart fixture and pass sleeps, not packer CPU. |
| C2/C3 namespace, size/read edges, repack of modified packed files | `w125_closure_private.sh REL OUT 17750` | **PASS**, 0 failures, no segment left |
| P5 modify matrix | `w125_p5_matrix.sh REL` | **PASS**, 2,000 files, `bad=0` before and after repack |
| Crash injection (admitted, uploaded, switched, gc_accepted, retire_accepted) | `w125_p7_crash.sh FAULT_REL` | **PASS** all 5: 600 files verified after restart, no fragment or segment left (at `admitted` this needs the new leader's bounded abandon sweep to cancel the crashed leader's pending segment). One writer got EIO when the leader exited at `admitted`, as the fault intends. |
| Modify under load, 600 s | `w125_modify_repro.sh REL OUT 17740 600` | 5 runs, alternating builds (ports 17740/17760/17780 branch, 17740/17770 base). Branch run 1: **3 transient read mismatches** at t≈220 s, all on one 3.6 MB file (not packed at that version) (`w2/d049/f004799` v6, correct size, wrong bytes; RC=1), final verify of 49,789 live files clean (`verified_bad=0`). Branch runs 2–3 and both base runs: `mismatch=0 errors=0`, final verify clean (~50,000 files each). **Unexplained, not attributed:** at 3.6 MB the file is above the 16 KiB packing threshold, so no changed code path selects, reads or rewrites that version; storage of that run was wiped before inspection. Treat as an open client-read item to reproduce on devel. |

All private clusters stopped themselves; `/tmp/pvo` and the run directories
were deleted afterwards. Raw output was not kept.

**NOTRUN:** live CPU A/B on orcd-mini under the user's tiny-file workload
(`scripts/orcd_mini_workload_profile.sh`, old build first) — the only test
of the profile percentages above; compaction of holed segments on a cluster
(covered by unit tests only); `.efs` and LSM findings 3–5 are unchanged.

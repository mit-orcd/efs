# ORCD tiny-file CPU profiles — October 22, 2026

**Verdict: analysis complete; performance fixes and A/B acceptance NOTRUN.**
Related work: W125 (packing), W126 (KV maintenance), and enabled `.efs`
indexing. This is a review of existing captures, not a new qualification run.
No deployment, workload, process or original capture was changed.

## Evidence and scope

Read through `ssh -A orcd-login` and nested `ssh fstor007`, without BatchMode.
Original evidence is on the shared filesystem:
`/home/erbmi1/orcd/scratch/efs/profile-tiny-20261022/`.
Each of `fcstor003`–`fcstor006` contains `oncpu-30s.perf.data`, `threads.txt`,
`top-self.txt`, `flat-self.txt` and `children-selected.txt`. Raw files stay there;
none are committed here. The saved summaries were checked against raw perf
reports. Despite its name, `threads.txt` groups by comm and is not a per-TID
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

The capture headers label the run October 22, 08:51:42 (timezone not recorded
in that display). Each records approximately 30 seconds at 499 Hz, event
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
initialization alone performs N+1 full validations, plus its other work.
This is a confirmed repeated-work mechanism, not a measured speedup estimate.

Recommendation: validate the pinned immutable image once, then validate each
reference against that validated image. Preserve all identity, bounds and
checksum guarantees; do not introduce a generally callable unchecked read or
reuse validation after image mutation. Test corruption and wrong references as
well as CPU cost versus record count.

### 2. Searching for abandoned packing work costs CPU too

On fcstor004, `pk_abandon` is 5.24% and its callback 4.71% inclusive.
`pk_group` calls it and `pk_abandon` starts an object-prefix scan from the
beginning. The callback decodes objects before checking phase, pending records
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
relaxation or disabled integrity checks are proposed. Fix validation, full-payload
correctness, restart checks and measured A/B gains remain NOTRUN.

# Private Intel: writer overlap and grouped durability — 2026-10-15

Follow-up to [durable storage isolation](intel-durable-isolation-20261015.md).
**PASS: 16 untraced and four diagnostic 1 GiB mounted writes, final fsync and
full SHA-256 verification; 12 local grouping cases / 49,152 records verified.**
No EFS source changes or production configuration/deployment changes. This is
measurement, not crash-durability or production rollout qualification.

## Mounted result

Two simultaneous clients, one on each Intel host, write separate nonzero 1 GiB
files through private TCP exports. Each case includes final application fsync;
full reread/hash verification and deletion are outside the write clock. Writers
per daemon follow 4, 8, 16, 4, restarting the same private stores between phases.
Two trials per client per phase. All four daemons use the same writer setting.

| Writers per daemon | Intel1 client, individual MiB/s | Intel2 client, individual MiB/s |
| --- | --- | --- |
| 4, initial | 37.31, 61.44 | 37.22, 59.77 |
| 8 | 78.53, 97.37 | 79.77, 91.92 |
| 16 | 80.52, 123.64 | 101.75, 97.22 |
| 4, return | 33.09, 59.70 | 33.75, 56.68 |

Pooling the four baseline observations per host gives medians 48.51/46.95 MiB/s.
Eight-writer medians are 87.95/85.84: **1.81x/1.83x**. Sixteen-writer medians
102.08/99.49 are **2.10x/2.12x** baseline, only about 16% beyond eight.
These are per-client rates, not a synchronized aggregate throughput measurement.
The return to four supports a concurrency effect, but first/second trial variation,
shared physical hardware, placement and cache state remain material confounders.
No human production workloads were paused in this experiment.

## Grouped durable completion: independent local model

Existing `efs-bench --bench principles`, ordinary files on each Intel `/data1`,
eight workers, 4096 records, 64 KiB nonzero payload plus 4 KiB checksum tail.
Groups run 1,4,16,16,4,1. Every file still receives fsync. A worker writes its
cohort first, fsyncs each file, then fsyncs its directory; operations complete only
after successful persistence of the cohort. No acceptance-only mode was used.

| Cohort size | Intel1 MiB/s (two runs) | Intel2 MiB/s (two runs) | p95 durable latency, ms | Sync calls/case |
| --- | --- | --- | --- | --- |
| 1 | 75.01, 74.55 | 75.65, 75.16 | 8.64–8.96 | 8192 |
| 4 | 99.07, 98.94 | 102.76, 103.83 | 22.40–22.91 | 5120 |
| 16 | 118.16, 110.28 | 112.86, 142.98 | 73.22–89.09 | 4352 |

Median gains are approximately 32–37% for four-record cohorts and 53–70% for
sixteen. These combine write-ahead within a cohort and directory-barrier
amortization; they do not isolate those effects. Every case retains 4096 file
syncs. Production already groups directory syncs, so these gains cannot be added
to the mounted writer gains or promised as an additional production improvement.
Per-worker directories and an independent harness also differ from production.

## Profiling and tracing

A separate eight-writer diagnostic uses two more 1 GiB runs/client, all verified.
Each host captures both private daemons and its private client: 15 seconds perf
at 99 Hz with call stacks, then 15 seconds strace of write/pwrite64/fsync/fdatasync.
Diagnostic timings are excluded from the matrix. Both perf captures succeeded;
strace timeout exit 124 is the intended bounded stop and detached the tracees.

Both hosts show 16 distinct threads doing 69,632-byte fragment writes and peak
16 overlapping successful fsync calls on those threads across their two daemons.
Intel1/Intel2 captured 26,730/26,706 completed worker fsync calls, averaging
2.816/2.873 ms. These combine file and directory syncs and both storage roots;
they must not be compared directly to earlier slow-root file-only measurements.
The parser joins unfinished/resumed syscalls by TID and uses syscall duration
for overlap. Capture-edge incomplete calls are excluded. Ptrace perturbs latency.

Perf reports hashing, copying, kernel I/O and XFS activity across the clients and
daemons, without a single overwhelming sampled CPU function. Samples from Intel
hybrid CPU PMUs are separate populations. CPU samples alone do not establish CPU
saturation or quantify time blocked on storage. The decisive evidence here is
the untraced writer-count comparison plus observed concurrent sync execution.

## Identity, reproduction and retained evidence

Source/binaries: unchanged production revision
`2e935cea075dfec45d25385bc8626c49fee67ba3`, build ID `2e935cea075d`, timestamp
`2026-10-15T03:42:00Z`; local checkout HEAD was `6dc879e35`; deployed binaries were reused unchanged. Daemon SHA-256 Intel1:
`3b65b51830ff63e21d1271d5f439a9bc688cbd4111435797ca2b0295c760f8ec`, Intel2:
`2833f63756909a1d70f905be6863a3de5b3bf7cdddf8bac2e8320c4b79189ad5`.
Existing principles binary/archive identity and physical disk details are in the
linked isolation report; the same binaries were reused without recompilation.

Four private nodes, 16 GiB quota each, two per Intel host on `/data1` and `/data2`,
ports 27432/27433. `--storage-engine files --direct-io --writers N`.
Client/server environment: `EFS_TRANSPORT=tcp EFS_COMMANDS=0
EFS_COMMANDS_SERVER=0 EFS_PACKED_WRITES=0 EFS_EXPIRY=0 EFS_MD_RAFT_N=4`.
Cluster commands policy was set to mask zero. No containers or expiry feature.

Use `scripts/intel_w100_private.py` with a fresh run label and existing production
release, adding the existing daemon argument `--writers N` to its launch list.
The temporary controller varied only N, using start once and stop/resume between
phases. Preserve any existing private fixture and restore it afterward. For each
phase concurrently on both hosts run the existing committed workload:

```sh
python3 tests/measure/intel_dd_sync_batch.py PRIVATE_MOUNT LOCAL_ARTIFACT_DIR candidate --mib 1024 --repeats 2
```

This invokes `dd if=NONZERO_FIXTURE of=NEW_OWNED_FILE bs=1M count=1024
conv=fsync,excl status=none`. Keep the fixture unchanged across phases.
For local grouping, create an empty owned directory per case and run:

```sh
efs-bench --bench principles --storage EMPTY_OWNED_DIR --layout files --rw write --policy allocating --persist batch --directory-sync worker --objects 4096 --payload-size 65536 --qd 8 --batch-size N --direct-io
```

Use `--persist each --batch-size 1` for the baseline. Require BENCH_OK,
errors=0, verified_objects=4096 and empty case directory after each run.
Profile with `perf record -F 99 -g -p PRIVATE_PID_LIST -o perf.data -- sleep 15`;
trace separately with `timeout -s INT 15 strace -f -ttt -T -s 16
-e trace=fsync,fdatasync,pwrite64,write -o trace.txt -p PID ...`.

Raw output, exact temporary controllers, trace parser and SHA256SUMS:
`/Users/mike/Documents/Codex/2026-10-10/n-x20/artifacts/intel-overlap-20261015/`.
Remote captures/fixtures: `/data1/efs/overlap-20261015/` on both Intel hosts.
Random fixture hashes: Intel1
`eb845af7d1d99f31e14c2efce77887b2c45d811e23fa14ecbdce89648af5ccbf`, Intel2
`5742765b3e960ad28363954b60159d65fc7735b21538922dadbebfe240710863`.

All benchmark output files were removed by successful harness cleanup. Local
case directories were emptied and removed; random input fixtures, raw logs and
stopped experimental stores are retained. Experimental clients/daemons stopped;
original private `deadline-posix` stores and mounts restored. Original private
PIDs now Intel1 3907363/3907364/3907648, Intel2 4172218/4172219/4172314.
Production daemon/client PIDs remain 3880449/3880456/3881522 and
4146981/4146988/4147513. No production rollout, human-job stop or binary change.

## Recommendation and remaining gates

Eight writers is the strongest low-complexity candidate: measured approximately
1.8x per-client throughput without changing code or weakening barriers. Sixteen
adds less and warrants fairness/resource testing before selection. Before rollout,
qualify sustained writes, small-file foreground latency, bounded queues, error
propagation and cancellation under the higher concurrency. The present short
runs are not that qualification. One-stream AMD dd was not tested here.

Broader grouped completion remains a design candidate with clear latency cost,
not an implemented or end-to-end qualified change. Crash/power-fail recovery,
full POSIX suites and fault injection were NOTRUN. No claim that one file fsync
persists another file, or that the October 8 regression hypothesis is proven.

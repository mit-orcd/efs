# Intel /data1 durability isolation — 2026-10-15

Follow-up to [PUT stage measurement](intel-put-stages-20261015.md).
**PASS: 60 existing-benchmark cases and 12 shared-directory controls, 294,912
full records verified, all case scratch directories emptied.** Two preliminary
128-record smoke cases also passed. No EFS runtime code, configuration, daemon,
mount, filesystem tuning or deployment change. Measurement scripts only.

## Findings and next decision

More concurrent durable I/O helps on these roots, even with all writers using
one directory. The current production four-writer limit is therefore worth
qualifying before a pipeline rewrite. The strongest controlled result is the
palindromic shared-directory comparison below: eight workers provide about
1.84 times four-worker throughput on both hosts, retaining every file and
directory barrier. This is **local fragment throughput**, not a measured or
promised increase in mounted EFS dd throughput.

The next low-impact experiment is the existing `efsd --writers 8` option on
private Intel, retaining the same durability/transport/features and client memory
limits. Compare unchanged binaries at four versus eight writers with nonzero
mounted dd including final fsync, small-file foreground latency/fairness, and
concurrent-write integrity. Only roll out after confirming an end-to-end gain.
The option exists in `src/server/efsd.c`; no new queue or format is required.
Production remains at four writers. Sixteen workers and batch deferral should
not be adopted solely from these local numbers: their latency/resource tradeoffs
need end-to-end qualification.

The experiment **does not establish metadata contention as the primary cause**.
The MD5 crawl is mixed metadata/data-read traffic; pausing it cannot isolate
metadata alone. A separate hot-set stat-only load did not reduce measured write
throughput, and resumed-crawl runs were faster than earlier quiet runs on Intel2.
Time/background/filesystem-state variation is substantial. Thus the earlier
single pause comparison cannot quantify a causal metadata penalty. Cold
namespace metadata and production daemon/background contention remain possible.

## Identity and scope

The production source remains `2e935cea075dfec45d25385bc8626c49fee67ba3`,
TCP/direct I/O, ordinary files, 400 GiB per node. `.efs` client/server and
expiry remain disabled. Intel1 n1/n2 PIDs 3880449/3880456;
Intel2 n3/n4 4146981/4146988, ports 17432/17433, unchanged. Private Intel services
were left running and untouched. AMD's independent client was not redeployed.

Only `/data1` was benchmarked: Intel1 `/dev/nvme0n1`, Intel2 `/dev/nvme1n1`.
Both report model `PCIe SSD`, firmware `ECFM22.7`, write-back cache, scheduler
`none`, XFS `noatime,logbufs=8,logbsize=256k`; kernel
`6.12.0-211.64.1.el10_2.x86_64`. About 34%/32% occupied, respectively, with over
630 GiB free. The device was not exclusively reserved: daemons/background work
remained active even during the phase called quiet. Accessible hwmon inspection
did not expose NVMe temperature readings; no thermal or hardware-fault conclusion.

The existing `efs-bench --bench principles` was compiled **separately**, from an
archive of the exact deployed source, under `/data1/efs/durable-isolation-20261015/source`.
No installed binary was overwritten. Build: `make -j4 efs-bench`, native GCC
`-O3 -g -fno-omit-frame-pointer -march=native -mtune=native`. Exported source lacks
`.git`, so embedded version/build ID says `unknown`; source provenance is the
commit plus archive SHA-256, not that embedded string.

* Source archive SHA-256: `8d324607af5d1fedd4e0aafcae2ac4f5447d8bacb097bcec8bfae388783322a4`
* Intel1 benchmark binary: `28ba945237c5960876bcc0afb2324e94d9801cd4acf0c2c91af66cec8dd245c6`
* Intel2 benchmark binary: `b988c7eb94ed9f6aaea5186b88950050e099d426ca608ed13eb902d52e5e1aa4`

## Existing benchmark: durability and contention

Each case writes 4,096 independent records: 64 KiB nonzero payload plus a 4 KiB
BLAKE3 checksum tail, 272 MiB physical data/256 MiB payload. `--direct-io --rw write --policy allocating --persist each --directory-sync worker`.
Every file is synced, and its worker directory synced, before operation completion:
8,192 sync calls/case. Verification reads/checksums and cleanup are outside timing.
Two repetitions per concurrency level with reversed/counterbalanced order.
No expiry or network data path are exercised.

Payload MiB/s medians of two repetitions:

| Host / background | 1 worker | 4 workers | 8 workers | 16 workers |
| --- | ---: | ---: | ---: | ---: |
| Intel1 / original crawl, before | 15.88 | 30.82 | 58.21 | 73.80 |
| Intel1 / crawl paused | 25.03 | 49.66 | 75.94 | 101.33 |
| Intel1 / stat-only | — | 55.86 | — | 102.39 |
| Intel1 / original crawl, resumed | — | 43.59 | — | 85.78 |
| Intel2 / original crawl, before | 9.64 | 21.79 | 31.36 | 41.04 |
| Intel2 / crawl paused | 13.61 | 24.87 | 35.51 | 38.37 |
| Intel2 / stat-only | — | 39.38 | — | 56.72 |
| Intel2 / original crawl, resumed | — | 43.47 | — | 87.60 |

Quiet does not consistently beat loaded, particularly on Intel2; do not combine
these samples into a precise metadata-contention percentage. Some loaded Intel2
cases spent tens of seconds in untimed verification reads. They completed and
verified; that extra time is not included in the throughput values.

The stat-only phase ran four threads on xorinox using six existing immutable
files. It performed **80,658 successful `os.stat` calls in about 54 seconds**,
checking inode/size identity, with no file-body reads or writes. The mount's
attribute/entry timeout is zero in source, but this is a native-stat call count,
not a directly observed RPC count. The hot six-file set cannot represent a cold
million-file namespace. The human MD5 crawl was stopped only temporarily with
a ten-minute automatic-resume guard, resumed as the same PID 727786 with output
preserved, and the guard retired after explicit resume. The owned stat load ended.

### Separate diagnostic controls (single cases)

| Quiet control, four workers | Intel1 MiB/s | Intel2 MiB/s |
| --- | ---: | ---: |
| New files, sync each | 49.66* | 24.87* |
| Batch 16, still sync every file | 99.80 | 44.89 |
| Pre-existing files, sync each | 54.33 | 25.89 |
| Acceptance only, no sync inside timing | 1,568.89 | 957.97 |

\* Two-case median reference; other controls are single cases.

Batch mode writes ahead, then syncs each file and one directory per cohort:
4,096 file + 256 directory syncs. It changes both write-ahead overlap and directory
amortization, so it does not isolate either effect. Quiet batch p95 operation-to-
durable-completion latency was 46.85/101.38 ms versus about 7/14 ms in the first
four-worker sync-each trials. Production already groups some directory barriers;
this local batch benefit cannot be transferred directly to a new implementation.

Overwrite's modest benefit does not identify filename creation as the sole cost.
The acceptance-only control flushes after timing and verifies, but its quoted
rate is **not durable throughput**. It shows the large cost of frequent barriers
relative to short-window acceptance; it is not a recommendation to remove them.
These small tests do not establish sustained NAND bandwidth or power-loss safety.

## Shared-directory control: remove a layout confound

The existing benchmark creates one directory per worker, so changing worker count
also changes directory layout. `scripts/intel_shared_dir_durability.py` removes
that confound: all workers create unique files in **one common directory**, use
O_DIRECT, fsync each file and the shared directory, and verify every full record.
It uses a deterministic nonzero 64 KiB payload with per-record identity and a
4 KiB SHA-256 tail. This is an independent syscall model, not EFS's on-disk format.
It has Python/extra hashing overhead and differs from production's directory
barrier grouping. Compare trends within this control, not absolute rates across
implementations.

The crawl and owned stat load were both paused/stopped for these six cases per
host, in order **4, 8, 16, 16, 8, 4**. Medians of two runs:

| Concurrent workers, shared directory | Intel1 MiB/s | Intel2 MiB/s |
| --- | ---: | ---: |
| 4 | 34.20 | 34.11 |
| 8 | 62.79 | 62.89 |
| 16 | 91.44 | 92.29 |

The four-worker endpoints remain close (Intel1 33.85/34.55; Intel2 34.24/33.99),
and eight-worker repeats also agree. This is the clearest evidence that more
I/O overlap can improve **durable local writes**, without increasing directory
count or omitting barriers. CPU usage grows from about 0.26–0.29 to 0.79–0.85
CPU cores, so higher levels may also expose harness CPU limits. No production
writer-count change, EFS dd speedup, crash test or new pipeline is claimed.

## Reproduction

Use fresh dedicated scratch directories on the same underlying `/data1`
filesystem, **outside daemon storage roots and outside FUSE mounts**. Never pass
`/data1/efs/n1`, `/data1/efs/n3` or a populated root to a benchmark. Existing
benchmark rejects nonempty scratch roots. Peak test data is about 272 MiB/host
per case, plus the isolated build. Successful cases remove only their own files.

Archive/build the exact source without modifying a running release:

```sh
git archive 2e935cea075dfec45d25385bc8626c49fee67ba3 Makefile src include deps scripts > source.tar
# On each host, extract into a NEW dedicated source directory, then:
make -j4 efs-bench
```

Run the committed measurement wrapper on each host (binary and phase paths are
absolute); the phase directory must already exist and be empty:

```sh
python3 scripts/measure_intel_durability.py BINARY EMPTY_PHASE_DIR loaded-before
python3 scripts/measure_intel_durability.py BINARY EMPTY_PHASE_DIR quiet
python3 scripts/measure_intel_durability.py BINARY EMPTY_PHASE_DIR stat-only
python3 scripts/measure_intel_durability.py BINARY EMPTY_PHASE_DIR loaded-after
```

Each invocation prints commands, results, identity, full-check verdicts and disk
snapshots as JSONL. The `loaded-before` and `quiet` phases run all four levels
twice plus three controls. `stat-only` and `loaded-after` run four/sixteen twice.
Each subprocess has a 180-second limit including setup/verification; unexpected
failure preserves residual scratch for inspection rather than recursive deletion.
Disk snapshots bracket the **whole command**, including untimed preparation,
verification and cleanup; do not divide their deltas by timed-write duration.

The authorized existing crawl must remain in the specified state throughout each
phase. Pause/resume the identified PID with a finite automatic-resume guard; do
not kill/restart it or discard its output. For stat-only use:

```sh
python3 scripts/intel_stat_pressure.py /data1/efs/intel-mnt \
  --seconds 180 --stop-file UNIQUE_HOST_LOCAL_STOP_FILE
# Signal early completion by creating only that owned stop file.
```

The script requires the six documented existing tiny-file controls. It makes no
file-body reads and rejects identity changes. Then stop that load, keep the human
crawl paused briefly, and run:

```sh
python3 scripts/intel_shared_dir_durability.py DEDICATED_SHARED_TEST_PARENT
```

Resume the crawl before `loaded-after`; retire the owned guard after confirming
resume. All 72 cases and all preflight checks passed in this run. The three Python
measurement scripts also pass syntax compilation. No broader POSIX/sanitizer or
crash/restart suite is required or claimed for this no-runtime-change experiment.

## Evidence and final state

Raw artifacts outside Git:
`/Users/mike/Documents/Codex/2026-10-10/n-x20/artifacts/intel-durable-isolation-20261015/`.
Intel build/empty scratch directories: `/data1/efs/durable-isolation-20261015`.
Xorinox measurement script/stop marker:
`/data1/efs/intel-client/durable-isolation-20261015`.

`SHA256SUMS` records source archive, smoke identity and all result files.
Shared-control result SHA-256:

* Intel1: `0dcfc190f83d4b1d9f84d9ff48fa0dbf5ed73419bcf24b163a214bc7066398e1`
* Intel2: `3dbf2de3b588bf65804497c5c37de78d6ceeb58b4f616bbdd2ef9fff7c42cc06`

All five phase scratch directories per host are empty, no efs-bench remains,
the original crawl is running, and its resume guard and stat-only load are gone.
The production release symlinks/PIDs remain unchanged. Build binaries, source and
raw measurement output are retained separately; no raw captures/payloads enter Git.

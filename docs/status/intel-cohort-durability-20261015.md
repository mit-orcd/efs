# Grouped fragment durability: design and local benchmark — 2026-10-15

**Superseded the same day by [asynchronous PUT
acknowledgement](intel-async-durability-20261015.md) (`d8af2c325`), which
removed the `EFS_PUT_COHORT` mode described below.** The measurements remain
valid evidence for why per-fragment barriers limited writes.

**Update, same day: implemented as an opt-in flag in `5cef06ac4` (scoped PASS,
crash qualification NOTRUN).** With `EFS_PUT_COHORT=1` on the Intel cluster,
four concurrent 4 GiB writers from xorinox reach 119–132 MiB/s versus
94–101 MiB/s without it, with less than half the device flushes per MiB;
single-stream writes 169–170 versus 146–149 MiB/s. See
[end-to-end results](#end-to-end-5cef06ac4). The cluster was returned to the
flag-off state afterwards.

**Local model PASS.** On both Intel `/data1` roots,
one filesystem barrier per cohort of fragment files (group commit with a short
gather window) gives **157–164 MiB/s at 8 writers and 282–301 MiB/s at 16**,
against 64–66 and 93–95 MiB/s for the production barrier model, with lower
per-record latency (p50 3.0–3.5 ms against 7.6–10.7 ms) and 4–8x fewer device
flushes. All 147,456 records verified. No EFS code, configuration or
deployment changed. Crash/power-loss behaviour of the proposed barrier is not
tested. Follow-up to [durability isolation](intel-durable-isolation-20261015.md)
and [PUT stages](intel-put-stages-20261015.md); no dedicated W item yet.

## Why

Profiles of 4 GiB `dd conv=fsync` writes on build `49d721a4f180`
([read fixes page](intel-read-metadata-cache-20261015.md) for the build and
environment) show the write ceiling (about 138 MiB/s from one client) is the
per-fragment durability barrier, not CPU or bandwidth:

- dd is blocked 85–90% of the time, in a few 0.1–1.7 s waits while the client's
  write buffer drains; the client's put threads mostly wait for server ACKs.
- Intel writer threads spend 55% of their blocked time in `fsync` (37% idle).
  CPU goes to O_DIRECT write and extent allocation (15%), creating each fragment
  file (13–14%), verifying the client BLAKE3 (14–16%), fsync (10%), XFS
  transaction commit (9%).
- The `/data1` devices are 75–79% busy while writing about 56 MB/s, with
  800–1,000 flushes/s: about one device flush per 64 KiB fragment.

Production makes each fragment durable with its own `fsync(file)`
(`finish_fragment_put` → `finish_shard_write(..., 1, 0)`), followed by a
grouped directory barrier (`sync_fragment_parents`,
`fragment_directory_group_barrier`). Each file fsync forces the XFS log and a
device cache flush.

## Design (implemented in `5cef06ac4`, opt-in)

**Per-root durable cohort (group commit).** For a PUT that created a new
fragment file:

1. Write the fragment with O_DIRECT and close it, without a file fsync.
2. Join the storage root's cohort: take a ticket. If no barrier is running,
   become the leader; otherwise wait.
3. The leader waits up to a bounded gather window for more members, closing the
   cohort early once every PUT currently writing on that root has joined (so a
   lone PUT never waits the full window). It then runs **one barrier**:
   `syncfs(root_fd)` followed by `fdatasync(probe_fd)` on a clean per-root probe
   file.
4. The barrier covers every ticket issued before it started. Members ACK only
   after it returns success. If it fails, every member of that cohort fails with
   `EFS_ERR_IO` (no partial ACKs); the client retries, which takes the existing
   overwrite path.

**Why both calls.** On XFS, `syncfs` forces the log; the first log write after
the force carries a cache-flush, so completed O_DIRECT data and every journalled
create, directory entry and extent allocation become durable together. When the
log has nothing to write, `syncfs` may not flush the device cache; `fdatasync`
on a clean file makes `xfs_file_fsync` issue an explicit cache flush in that
case. The pair is one or two flushes per cohort instead of one or more per
fragment. `syncfs` reports writeback errors through the superblock error
sequence (Linux 5.8+), sampled per open file, so the root descriptor must stay
open for the daemon's lifetime and be checked on every barrier.

**As implemented.** `src/server/durable_cohort.h` holds the group commit;
`store.c` adds `fragment_cohort_for` (per-root lookup, XFS check, persistent
root descriptor, one log line per root) and `shard_io_put` (pairs every
`durable_cohort_begin` with exactly one join or abort). Differences from the
proposal: the barrier is `syncfs(root_fd)` alone, because only files this PUT
created join, so every cohort has journalled creates and the log force always
writes (and flushes); no probe file is needed. Cohort PUTs open with `O_EXCL`
and fall back to the unchanged per-file barrier when the name exists.
`EFS_PUT_COHORT_GATHER_US` (default 2000, max 20000) sets the gather window.

**Scope limits for a first implementation.**

- Opt-in flag, XFS roots only (check `statfs` at root attach); other filesystems
  keep per-file fsync.
- Only newly created fragments (`O_CREAT|O_EXCL` success) join a cohort.
  Overwrites keep their per-file `fsync`; the probe flush would also cover them,
  but they are rare and keeping them unchanged narrows what needs qualification.
- The ancestry proof for newly created fragment directories is journalled and
  covered by the same log force; keep the existing directory barrier on the
  fallback path.
- `syncfs` also writes back any dirty page cache on that filesystem. On these
  roots the daemon's other durable state already uses its own barriers, but a
  barrier's latency then depends on unrelated buffered writers; measure this
  before rollout (not done here).

**Gates before enabling.** Fault injection for a failing barrier (all members
fail, retries succeed, no ACK without durability); crash tests (daemon kill and
host power-cut or `dm-flakey` drop of unflushed writes) proving every ACKed
fragment survives; mounted end-to-end dd with final fsync and SHA-256 at four
and eight writers; small-file latency/fairness under a concurrent large writer;
POSIX/peer suites; and the same on ORCD before any production change.

## Local benchmark

`tools/bench_cohort_durability.c` models a fragment PUT: each record is a new
file in a fresh case directory, created `O_CREAT|O_EXCL|O_DIRECT` and written as
one 64 KiB payload plus 4 KiB tail (68 KiB), the on-disk shape of an EFS fragment.
A record counts as acknowledged only after its barrier mode completes; latency is
open to acknowledge. Modes:

| Mode | Barrier per record |
| --- | --- |
| `groupdir` | `fsync(file)`, then a group-committed `fsync(dir)`: the production model |
| `batch` | each worker writes 8 files, fsyncs each, then the directory once (the earlier batch control) |
| `syncfs` | group-committed `syncfs(dir)` |
| `syncfs+flush` | group-committed `syncfs(dir)` + `fdatasync(probe)` (the proposed barrier) |
| `syncfs+flush min=W wait=2000us` | as above; the leader waits up to 2 ms for W members |

Group commit: a waiting record whose ticket is not covered either leads (one
barrier covering all tickets issued so far) or waits for the running barrier.
After timing, every record is read back and compared, then all files and the
case directory are removed. Device counters (`/proc/diskstats`) bracket the
timed region only.

**Environment.** intel1 `/dev/nvme0n1` and intel2 `/dev/nvme1n1` (`/data1`,
"PCIe SSD", XFS `noatime,logbufs=8,logbsize=256k`, write-back cache), kernel
6.12.0-211.64.1.el10_2, Rocky Linux 10.2. Scratch
`/data1/efs/evidence/cohort-bench-20261015/run`, outside daemon roots. The Intel EFS
cluster (`49d721a4f180`) and the private w100 cluster were running but idle;
the devices were not reserved. 4,096 records (256 MiB payload) per case, each
case twice in palindromic order, both hosts concurrently on separate devices.
Tool SHA-256 `cd109ce31ead7c1534acb8b2193223025fd2c7bb9ec348c04b7fa0636d9dca0e`
(built `cc -O2 -Wall -Wextra -Werror -pthread`).

**Results** (median of two; per-run values in raw evidence; every case
verified 4,096/4,096):

| Case | intel1 MiB/s | intel2 MiB/s | p50 / p95 / p99 ms (intel1) | flushes per record | device busy |
| --- | ---: | ---: | --- | ---: | ---: |
| groupdir, 8 writers | 65.9 | 64.0 | 7.63 / 10.24 / 11.40 | 1.12 | 97% |
| batch, 8 writers | 93.6 | 95.6 | 34.59 / 50.82 / 58.77 | 1.09 | 100% |
| syncfs, 8 writers | 103.3 | 98.5 | 4.73 / 6.65 / 7.65 | 0.27 | 92% |
| syncfs+flush, 8 writers | 94.5 | 93.4 | 5.26 / 6.47 / 7.60 | 0.50 | 91% |
| syncfs+flush, 8 writers, min 8, wait 2 ms | **163.7** | **157.0** | 3.02 / 3.75 / 4.06 | 0.25 | 82% |
| groupdir, 16 writers | 95.1 | 93.2 | 10.55 / 13.81 / 15.94 | 1.00 | 97% |
| batch, 16 writers | 105.9 | 104.4 | 63.96 / 89.70 / 100.90 | 0.99 | 100% |
| syncfs+flush, 16 writers | 156.0 | 155.2 | 6.34 / 7.50 / 9.14 | 0.25 | 90% |
| syncfs+flush, 16 writers, min 16, wait 2 ms | **300.8** | **282.3** | 3.26 / 4.24 / 4.75 | 0.13 | 80% |

Without a gather window the natural group commit forms cohorts of about 4 (8
writers) and 8 (16 writers). Waiting for a full cohort halves flushes again,
raises throughput and *lowers* latency, because the device is no longer
saturated by barriers. Batch mode, which keeps a fsync per file, gains only
40–45% and multiplies latency. Two runs of each case agree within 6% except
intel2 batch at 16 writers (99.0/109.8).

## End-to-end (`5cef06ac4`)

Release `5cef06ac45a0` (built 17:42Z; efsd SHA-256 intel1
`a3a872dcefdf7a0ca4883ac3e348abd8dff3a6aea4a98d0250d3f316c3c84f30`, intel2
`119ee53c72806830a92c7943379245f45e238c9f802317495cfbc7693c540ed2`; xorinox
efs-fuse `ae7f6c752dee581dea44a96028a2f6c14a12db46b8ec7fd962043d58c8e18443`)
was rolled out to intel1, intel2 and xorinox with the standard procedure
(64-sample preserved-file control checks before and after, all passing), first
without the flag, then with `EFS_PUT_COHORT=1` in the daemons' launch
environment (confirmed in `/proc/<pid>/environ`; each daemon logged
`store: put cohort root=... enabled gather_us=2000` for its root; all four
roots, including the `/data2` home-LV roots, are XFS). Same binaries, same
client, idle cluster otherwise. Workloads from xorinox with GNU dd
`bs=1M conv=fsync,excl` from a 4 GiB random fixture; every output verified by
SHA-256 and removed.

| Workload | Flag off | Flag on |
| --- | --- | --- |
| One 4 GiB writer, three runs | 145.8 / 149.0 / 148.7 MiB/s | 135.3 / 170.3 / 168.9 MiB/s |
| Four concurrent 4 GiB writers, two runs | 100.5 / 94.3 MiB/s aggregate | **132.1 / 119.5 MiB/s** |
| Device flushes, all four roots, per MiB written (4 writers) | 32.7 / 34.3 | **14.9 / 16.0** |
| `/data1` flushes per run, n1 / n3 | 120.5k–125.2k / 124.3k–129.2k | 42.5k–44.0k / 47.3k–51.1k |
| `/data1` busy, n1 / n3 | 89.6–91.6% / 95.0–95.5% | 85.1–86.1% / 88.6–89.0% |

The first flag-on single-stream run (135.3) followed the restart; the next two
were 14–15% faster than flag off. With four writers the gain is 30% (medians
97.4 → 125.8 MiB/s). Four writers still total less than one stream and `/data1`
stays 85–89% busy, so something besides fragment barriers limits concurrent
writes next (not isolated here; per-cohort sizes were not logged).

Raw (not in Git): `~/git/devops/profiling/runs/20261015-cohort-e2e/`
`off/write-baseline.jsonl` (`b634a85ac78d4dfc983b7c23749a5809c4a2b7c610c24ac6aaeb68dd027dc6c6`),
`off/write-parallel.jsonl` (`8c805b31bf2a0da080874fa14485267da1cc4b286a16c70ebd3213c7e0933576`),
`on/write-baseline.jsonl` (`3f80786d70a5e9ec38318d544a823dcf4fddb518864926ffdb8c1dcfb091ae57`),
`on/write-parallel.jsonl` (`6fa7f67c67fed268e483e885a2ebf7428e950c833024c6184a1464cdcd39a121`);
rollout logs under `20261015-rollout-5cef06ac45a0/` (`cohort-off`, `cohort-on`,
`cohort-off-restore`). Driver: `~/git/devops/profiling/efs_dd_profile.py
write-baseline` and `write-parallel --streams 4` with `EFS_PROF_CLIENT=xorinox_efs`.

**Tests.** `tests/test_durable_cohort.c` (group commit, a failed barrier fails
only its own cohort, arrivals during a barrier form the next set, early and
bounded gather, abort; clean under ASan/UBSan and ThreadSanitizer) and new cases
in `tests/test_store_durability.c` on the production PUT path with an injected
barrier (new file: no fsync and one cohort barrier; existing name: per-file
barriers; failed barrier fails the PUT and its retry overwrites; open failure
releases its count; no root without the flag). All 317 `make test` commands
pass on xorinox except the two pre-existing failures
(`tests/test_lookup_replacement.py`, architecture regeneration).

## Limitations

- A model, not EFS: no network, checksum verification, quota, metadata REPORT or
  concurrent reads. The 2.5–3x local gain will not translate one-to-one into
  mounted dd throughput; the client buffer drain and REPORT path then matter.
- With a constant number of writers the full gather window always fills. Real
  load varies; the "close when every active writer has joined" rule must be
  measured with uneven arrival.
- No concurrent buffered writer on the same filesystem during `syncfs`.
- No crash or power-loss testing; barrier failure is tested only with an
  injected failure. The durability argument above is from XFS and Linux
  behaviour, not from a test on these devices. Keep the flag off on clusters
  holding data that matters until the crash gates pass.
- Shared devices with idle daemons; small-sample medians.

## Reproduce

On each Intel host, with EFS idle, from a source tree containing
`tools/bench_cohort_durability.c`:

```sh
mkdir /data1/efs/cohort-bench-NEW        # must not exist; never a daemon root
scripts/measure_cohort_durability.sh /data1/efs/cohort-bench-NEW 4096 > results.jsonl
```

The script builds the tool in a temporary directory, runs the nine cases twice
in palindromic order (about 3 minutes on these hosts) and prints a header plus
one JSON line per case. Every case removes its own files and directory; the
scratch parent is left empty. Peak usage is 272 MiB.

## Raw evidence (not in Git)

`~/git/devops/profiling/runs/20261015-cohort-durability/` on the operator Mac:
`intel1_efs.jsonl` (`11fc095154a98050833b45b81f6b13c28eaa0c29a0114d0188684efb18666b2a`),
`intel2_efs.jsonl` (`071260e05ad1404b5a53eb4bd243dafa0da3959992be29fd1495e9cccff91151`).
Tool and runner copies remain under `/data1/efs/evidence/cohort-bench-20261015/src` on
each Intel host; its `run` directory is empty.

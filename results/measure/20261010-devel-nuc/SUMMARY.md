# 20261010-devel-nuc — NUC closure session (W70/W78/W79/W80/W96 + W97/W111 investigation)

## Purpose and linked item
Three-hour exclusive-NUC closure session on branch `devel-nuc`, starting at
devel `746ae9f0`; session window 2026-10-11 03:41–06:41 UTC (Oct 10 EDT).
Linked status page: `docs/status/devel-nuc-closure-20261010.md`. The
persistence-latency investigation begun here as "W97" was re-tracked as W111
(`docs/status/w111-nuc-persistence-investigation-20261011.md`); dated W97
labels in the raw files are historical.

## Verdict
PASS for the five named closures — W70, W78, W79, W80, W96 — with every
planned private gate and drive control reaching a terminal successful verdict
(all `closure/*-exit.txt` markers are 0). W97/W111 remains an OPEN
investigation, not a closure: eight clean cells, four profiles and four
written-ring cells pass. Sanitizer gates pass: ASan/UBSan WAL sticky-failure
and reopen recovery PASS; Valgrind WAL run reports 0 errors / 0 leaks.
Service restoration and bounded existing-data reads pass; installing the newer
tested binaries remained approval-blocked.

## Source commit and build identity
- Branch `devel-nuc` from devel `746ae9f099351624f5c38fa872dcc6f4daa5e7c3`;
  named NUC engine build `3df1931032b8-nuc`; production C/header/Makefile code
  unchanged from the captured devel tip (early branch commits add measurement
  and operator acceptance tests).
- `source-verification.json`: 176 paths checked, zero mismatches vs base
  `746ae9f0`; final state in `source-verification-final.json`.
- Preserved installed service binaries: efsd/efs-fuse/efs-mgmt/efs-query/
  efs-bench `6b8f05ac` (branch devel-persistence-pressure, built
  2026-10-10T11:47:05Z on nuc), sha256s in `final-service.json`.

## Environment
- Host `nuc`; devices (`device-context.json`, 2026-10-11T04:46Z): SATA
  MK0800GEYKE (sda, write-through cache, mq-deadline) and Samsung SSD 970 EVO
  1TB (nvme0n1, write-back cache); nvme default_ps_max_latency_us=100000.
- Existing four-node stores on 127.0.0.1:17432–17435, quota 187.5G,
  `--direct-io`; both service mounts preserved and restored in `finally`.
  Transport TCP; `EFS_COMMANDS=0`. No reboot or drive-setting changes.

## Method
- Session controllers at run root: `nuc_closure.py` (ordered closure gates),
  `nuc_w96.py` / `nuc_w96_replay.py` (WAL reservation on/off ABBA batches on
  both physical drives, bounded allocation, exact replay), `nuc_sanitizers.py`
  (ASan/UBSan + Valgrind WAL gates), `nuc_wall_strace.py`, `nuc_written_ring.py`
  and `nuc_durable_written.py` (W97/W111 three-writer durable controls:
  growing/reset vs preallocated rings, reversed repeats, exact reopened
  blocks), plus `profile-controller-executed.py`.
- Fixture dirs: `full-fixtures/` (direct/buffered/wal-failure),
  `operator-fixtures/` (w78, w79 direct/buffered), `sanitizers/`, `w96/`,
  `written-ring/`, `allocation-profile/`, `closure/` (final terminal exits and
  logs). Measurements drain regular clients/daemons first and restore the
  installed service afterwards.

## Results
| Gate | Result |
|---|---|
| W70 quota reductions (direct/buffered × 1/2-root) | PASS |
| W78 PID/daemon retirement (native + real NUC startup/stop) | PASS |
| W79 mkfs aliases/retry/refusal (direct + buffered) | PASS |
| W80 retired QUERY_STATS unavailable reply, no false totals | PASS |
| W96 WAL reservation on/off, both drives, ABBA batches, replay | PASS |
| Sanitizers (ASan/UBSan, Valgrind WAL) | PASS, 0 errors |
| W97/W111 drive controls (8 cells, 4 profiles, 4 ring cells) | PASS, investigation open |

## Limitations and remaining gates
- W96 harness measures WAL principles (production `kv_wal.c`, compile-time
  reservation constant only), not end-to-end file creation; sync histograms
  have 1 µs bins with a 1 s overflow bin.
- W97 controls use buffered I/O + fdatasync (not O_DIRECT), do not model
  fragment/checksum files or the full EFS workload, and do not claim
  kernel-cold or power-loss coverage.
- W73/W64, W53 (keep/revert decision), W54/W38 (physical multi-host), W92
  large fixtures and RDMA placement (W106) explicitly remain outside this
  session.

## Evidence index (sha256 first 16 chars)
- `closure/run-exit.txt` `9a271f2a916b0b6e` — overall closure exit 0
- `closure/native-exit.txt` `9a271f2a916b0b6e` — native suite exit 0
- `closure/full-direct-exit.txt` `9a271f2a916b0b6e` — full direct gate exit 0
- `closure/full-buffered-exit.txt` `9a271f2a916b0b6e` — full buffered gate exit 0
- `closure/w96-wal-failure-exit.txt` `9a271f2a916b0b6e` — W96 WAL failure gate exit 0
- `w96/exit.txt` `9a271f2a916b0b6e` — W96 measurement exit 0
- `sanitizers/valgrind-wal-exit.txt` `9a271f2a916b0b6e` — Valgrind WAL exit 0
- `source-verification-final.json` `72a9bdfc73e8dcff` — final source provenance vs devel
- `final-service.json` `3d3877be017c0c29` — restored installed binaries/policy
- `device-context.json` `1d6c6747ae015fac` — drive models/cache/scheduler settings
- `evidence-sha256.json` `9f591527376f6be0` — run-wide evidence hash manifest
- `closure/allocation-summary.json` `c6b461c6308d861c` — bounded-allocation summary

# Summary — Existing durability batching measurement, NUC (2026-10-09)

## Purpose and linked item

Observational measurement of the deployed W93 WAL durability batching under a
sustained tiny-file workload — how many Raft entries each WAL sync actually
amortizes. No code, deployment, batching limit, filesystem option or durability
contract changed. Linked item: W93 (bounded follower durability batching) —
docs/status/durability-batching-profile-20261009.md and
docs/backlog/work-items.md ("Oct 9 production measurement"). Run added by
commit e11c6d3bb.

## Verdict

Measurement completed (observational run; no PASS/FAIL acceptance gate attached).
Twelve strace windows parsed with zero completed-syscall errors; collector ran
the full 300 s. Conclusion: deployed batching works but amortizes few entries
per sync under this workload (n2/n3/n4 mean 1.45/1.34/1.39 entries/sync);
persistence latency, not batch limits, is the supported limiter hypothesis.

## Source commit and build identity

- Deployed build `800122c5e86d` (branch devel-find-stats, built
  2026-10-09T05:13:47Z on nuc) — identity.json records unchanged daemon PIDs,
  commands and WAL descriptors.

## Environment

- NUC cluster, four daemons (PIDs 2312683/2314230/2315840/2317774), 127.0.0.1
  ports 17432–17435, TCP, direct I/O, `.efs` enabled with existing 1024 MiB cap,
  187.5 G quota per node.
- Backing devices: n1/n2 on SATA (/data1), n3/n4 on NVMe (/data2; WALs under
  /home/efs/additional-work-dir). /data1 XFS noatime logbufs=8 logbsize=256k;
  SATA `write through`, FUA 0, mq-deadline; NVMe `write back`, FUA 1, none
  scheduler (queue config read during collection; not changed).
- Workload: dedicated generator PID 2519952,
  `python3 /home/efs/tiny_files.py --max-depth 8 --max-files-per-folder 1000
  --max-files 50000 --workers 8 /data1/efs/mnt/tiny-batching-20261009`.

## Method

- Collector: `capture.py` (repo path results/measure/20261009-durability-batching/capture.py)
  samples host/disk/process counters every 5 s for 300 s
  (2026-10-09 05:49:55–05:54:55 UTC).
- Three waves trace n1/n2/n3/n4 sequentially, 6 s each, starting at 30/38/46/54,
  130/138/146/154 and 230/238/246/254 s (twelve w*-n*.trace files);
  `strace -f -ttt -T -yy -xx -s 16 --trace-fds=WALFD` on pwrite64/fdatasync and
  WAL allocation/truncation calls.
- Analysis: `analyze.py` reconstructs syscall entry/completion, assigns full
  framed entry writes completed before a sync start to that sync (conservatively
  to the next sync if during); drops first sync per trace, incomplete end calls
  and failed writes. Reproduce: `python3 analyze.py` → analysis.json/analysis.txt.
- Cleanup: no settings changed; test data retained; capture counts are not
  verified filesystem counts.

## Results

| Node / device | Interior syncs | Entries assigned | Entries/sync | Single-entry | Mean sync | Approx. call reduction |
|---|---:|---:|---:|---:|---:|---:|
| n1 / SATA | 654 | 654 | 1.00 | 100.0% | 0.130 ms | 0% |
| n2 / SATA | 1,976 | 2,875 | 1.45 | 74.5% | 0.113 ms | 31.3% |
| n3 / NVMe | 1,554 | 2,075 | 1.34 | 76.4% | 7.325 ms | 25.1% |
| n4 / NVMe | 1,158 | 1,615 | 1.39 | 73.1% | 8.006 ms | 28.3% |

- Largest observed batches: 7 (n3), 6 (n4) entries; ~163–175 WAL bytes/barrier
  on n3/n4. Only 8 observed n2 barriers span groups; none on n1/n3/n4.
- Rates: 96 files/s pre-trace (5–30 s); untraced steady intervals 21.6/23.1/24.0
  files/s — slowdown persists with strace detached.
- Devices: NVMe ~93% busy, write await 3.71–4.20 ms, flush await ~4.05 ms at
  only 6–7 MiB/s; SATA write await ~0.18–0.19 ms.

## Limitations and remaining gates

- Nodes traced sequentially, not simultaneously; they host different group
  traffic (n1 group0; n2/n3 groups 0+2; n4 group2). Entries ≠ files; call
  reduction is arithmetic, not a measured throughput or physical-flush gain.
- Syscall-boundary estimates, not instrumented internal `cover` values; strace
  perturbs scheduling; fdatasync count ≠ physical block flush count.
- Shutdown correction (discovered during the tracepoint rerun): the nohup
  generator inherited SIGINT ignored, so it did not stop at 300 s and continued
  toward its bounded 50,000-file target; absence of loader-exit.json was not
  proof of draining. Affects shutdown interpretation, not the 300 s measurement.
- Next gate: low-overhead counters for entries/bytes per durable watermark,
  hold duration, waiter depth, follower eligibility/flush reasons; controlled
  bounded-coalescing A/B on identical fixtures with idle/mixed p99, RSS/queue
  and crash/sync-failure gates. Device placement/XFS config comparisons separate.
  Fragment persistence remains a separate cost.

## Evidence index

| File | sha256[0:16] | Carries |
|---|---|---|
| README.md | 59b175bd3ffb0a03 | Full report (tables, method, correction) |
| analysis.json | 1dbdd0cdbe67445f | Cohorts, distributions, parser diagnostics |
| analysis.txt | c0e420b91b35711b | Per-trace summary lines |
| identity.json | bfe3bccbba8adca8 | Daemon PIDs/commands/WAL fds, build version |
| windows.json | 201b37c7b96f0cd4 | Rates and diskstat deltas per window |
| events.jsonl | 75278b057f5df03a | Collector event stream |
| progress.log | 7970554d5b09945f | Generator progress over the 300 s |
| capture.py | f32aff5b5bca9931 | Reproducible collector |
| analyze.py | 3a7b4e120df7c832 | Reproducible analysis |
